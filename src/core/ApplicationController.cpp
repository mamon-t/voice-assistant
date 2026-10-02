#include "ApplicationController.h"

#include "audio/Agc.h"
#include "audio/QtAudioCapture.h"
#include "commands/CommandDictionary.h"
#include "commands/CommandParser.h"
#include "config/ConfigManager.h"
#include "config/HotwordsManager.h"
#include "core/VoicePipeline.h"
#include "output/XdotoolInjector.h"

#include <QDebug>
#include <QFile>
#include <cmath>
#include <cstdint>

namespace {

QString commandDescription(const Command& cmd)
{
    switch (cmd.type) {
    case Command::Type::SetMode:     return QStringLiteral("режим: %1").arg(modeToString(cmd.mode));
    case Command::Type::DeleteWord:  return QStringLiteral("удалить слово");
    case Command::Type::DeleteLine:  return QStringLiteral("удалить строку");
    case Command::Type::NewLine:     return QStringLiteral("новая строка");
    case Command::Type::Space:       return QStringLiteral("пробел");
    case Command::Type::Punctuation: return QStringLiteral("знак: %1").arg(cmd.argument);
    case Command::Type::Unknown:     break;
    }
    return QStringLiteral("неизвестная команда");
}

}  // namespace

ApplicationController::ApplicationController(QObject* parent)
    : QObject(parent)
{
    // 1. Конфиг -----------------------------------------------------------------
    m_config = std::make_unique<ConfigManager>();
    m_audioDebugLog      = m_config->audioDebugLog();
    m_editCmdsInDictation = m_config->editingCommandsInDictation();

    // 2. Команды ----------------------------------------------------------------
    m_commands = std::make_unique<CommandParser>();   // словарь уже наполнен стандартом
    const QString customCommands = m_config->customCommandsPath();
    if (QFile::exists(customCommands)) {
        m_commands->dictionary()->loadFromFile(customCommands);
    }

    // 3. Подсказки пользователя -------------------------------------------------
    m_hotwords = std::make_unique<HotwordsManager>();
    m_hotwords->setConfigManager(m_config.get());
    m_hotwords->reload();
    connect(m_hotwords.get(), &HotwordsManager::hotwordsChanged,
            this, &ApplicationController::applyHotwords);

    // 4. Инжектор текста --------------------------------------------------------
    auto injector = std::make_unique<XdotoolInjector>();
    const QString method = m_config->injectorMethod();
    if (method == QLatin1String("xdotool")) {
        injector->setMethod(XdotoolInjector::Method::XdotoolType);
    } else if (method == QLatin1String("clipboard")) {
        injector->setMethod(XdotoolInjector::Method::Clipboard);
    } else {
        injector->setMethod(XdotoolInjector::Method::Auto);
    }
    injector->setTypingDelayMs(m_config->typingDelayMs());

    if (!injector->initialize() || !injector->isAvailable()) {
        emit errorOccurred(QStringLiteral(
            "Ввод текста недоступен: нужен xdotool (X11) и, желательно, xclip. "
            "Для Wayland/TTY требуется ydotool."));
    }
    m_injector = std::move(injector);

    // 5. Аудиоподсистема --------------------------------------------------------
    m_audioCapture = std::make_unique<QtAudioCapture>();
    if (!m_audioCapture->initialize()) {
        qCritical() << "Failed to initialize audio capture";
        emit errorOccurred(QStringLiteral("Failed to initialize audio capture"));
        return;
    }

    m_agc = std::make_unique<Agc>();
    m_agc->setTargetDb(-15.0f);
    m_agc->setAttackTime(0.05f);              // 50 мс
    m_agc->setReleaseTime(0.2f);              // 200 мс
    m_agc->setMaxGain(10.0f);
    m_agc->setMinGain(0.01f);
    m_agc->setNoiseGateThresholdDb(-35.0f);   // порог тишины -35 dB

    connect(m_audioCapture.get(), &IAudioCapture::audioDataReady,
            this, &ApplicationController::onAudioDataReady);
    connect(m_audioCapture.get(), &IAudioCapture::errorOccurred,
            this, &ApplicationController::onAudioError);

    // 6. Речевой пайплайн (VAD -> ASR -> пунктуация) -----------------------------
    if (!buildPipeline()) {
        return;   // ошибка уже отправлена сигналом из buildPipeline()
    }
}

ApplicationController::~ApplicationController()
{
    stopRecording();
}

// ---------------------------------------------------------------------------
// Построение пайплайна
// ---------------------------------------------------------------------------

bool ApplicationController::buildPipeline()
{
    // Родителя QObject не передаём: владельцем является unique_ptr
    m_pipeline = std::make_unique<VoicePipeline>(VoicePipeline::loadSettings(*m_config));

    connect(m_pipeline.get(), &VoicePipeline::errorOccurred,
            this, &ApplicationController::errorOccurred);
    // ВАЖЕН ПОРЯДОК: rawTextRecognized приходит раньше textReady —
    // по «сырому» тексту мы ищем команду, а по готовому вставляем текст.
    connect(m_pipeline.get(), &VoicePipeline::rawTextRecognized,
            this, &ApplicationController::onRawTextRecognized);
    connect(m_pipeline.get(), &VoicePipeline::textReady,
            this, &ApplicationController::onTextReady);

    QString err;
    if (!m_pipeline->initialize(&err)) {
        emit errorOccurred(QStringLiteral("VoicePipeline: %1").arg(err));
        m_pipeline.reset();
        return false;
    }

    applyHotwords();
    qInfo().noquote() << QStringLiteral("ASR-профиль: %1 | инжектор: %2")
                             .arg(m_pipeline->asrProfileName(),
                                  m_injector ? m_injector->backendName() : QStringLiteral("нет"));
    return true;
}

void ApplicationController::applyHotwords()
{
    if (!m_pipeline) {
        return;
    }

    // Подсказки пользователя + фразы команд: ASR проглатывает редкие слова,
    // а диктантные команды обязаны распознаваться дословно (тот же эффект,
    // что и с «точка»/«запятая»). Диктантные знаки VoicePipeline добавит сам.
    QStringList hw = m_hotwords ? m_hotwords->allHotwords() : QStringList();
    if (m_commands && m_commands->dictionary()) {
        const QStringList phrases = m_commands->dictionary()->phrases();
        for (const QString& p : phrases) {
            if (!hw.contains(p)) {
                hw << p;
            }
        }
    }

    m_pipeline->setHotwords(hw, m_config ? m_config->hotwordsScore() : 2.0f);
}

// ---------------------------------------------------------------------------
// Режимы
// ---------------------------------------------------------------------------

Mode ApplicationController::mode() const
{
    return m_mode;
}

void ApplicationController::startRecording()
{
    if (m_mode == Mode::Off) {
        if (m_agc)      m_agc->reset();
        if (m_pipeline) m_pipeline->reset();
        m_audioBuffer.clear();
        m_skipNextText = false;

        if (m_audioCapture) m_audioCapture->start();
        m_mode = Mode::Dictation;
        qDebug() << "Recording started";
        emit modeChanged(m_mode);
    }
}

void ApplicationController::stopRecording()
{
    if (m_mode != Mode::Off) {
        if (m_pipeline) m_pipeline->flush();     // вытолкнуть хвост последней фразы
        if (m_audioCapture) m_audioCapture->stop();
        m_audioBuffer.clear();
        m_mode = Mode::Off;
        qDebug() << "Recording stopped";
        emit modeChanged(m_mode);
    }
}

void ApplicationController::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }

    if (m_mode == Mode::Off && mode != Mode::Off) {
        if (m_agc)      m_agc->reset();
        if (m_pipeline) m_pipeline->reset();
        m_audioBuffer.clear();
        m_skipNextText = false;
        if (m_audioCapture) m_audioCapture->start();
    } else if (mode == Mode::Off) {
        if (m_pipeline) m_pipeline->flush();
        if (m_audioCapture) m_audioCapture->stop();
        m_audioBuffer.clear();
    }

    m_mode = mode;
    qDebug() << "Mode changed to" << modeToString(mode);
    emit modeChanged(m_mode);
}

// ---------------------------------------------------------------------------
// Профили ASR и подсказки
// ---------------------------------------------------------------------------

QStringList ApplicationController::asrProfiles() const
{
    return m_config ? m_config->asrProfileNames() : QStringList();
}

QString ApplicationController::activeAsrProfile() const
{
    return m_pipeline ? m_pipeline->asrProfileName() : QString();
}

bool ApplicationController::switchAsrProfile(const QString& profileName)
{
    if (!m_config) {
        return false;
    }
    if (!m_config->hasAsrProfile(profileName)) {
        emit errorOccurred(QStringLiteral("Нет профиля ASR: %1").arg(profileName));
        return false;
    }

    const Mode oldMode = m_mode;

    if (m_pipeline) {
        disconnect(m_pipeline.get(), nullptr, this, nullptr);
        m_pipeline.reset();      // модель выгружается, ОЗУ освобождается
    }
    m_config->setActiveAsrProfileName(profileName);

    if (!buildPipeline()) {
        m_mode = Mode::Error;
        emit modeChanged(m_mode);
        return false;
    }

    setMode(oldMode);
    return true;
}

void ApplicationController::reloadHotwords()
{
    if (m_hotwords) {
        m_hotwords->reload();     // -> hotwordsChanged -> applyHotwords()
    } else {
        applyHotwords();
    }
}

// ---------------------------------------------------------------------------
// Аудио
// ---------------------------------------------------------------------------

void ApplicationController::onAudioDataReady(const QByteArray& data, int sampleRate)
{
    m_audioBuffer.append(data);

    while (m_audioBuffer.size() >= TARGET_CHUNK_SIZE) {
        const QByteArray chunk = m_audioBuffer.left(TARGET_CHUNK_SIZE);
        m_audioBuffer.remove(0, TARGET_CHUNK_SIZE);

        const QByteArray processed = m_agc ? m_agc->process(chunk, sampleRate) : chunk;

        if (m_audioDebugLog) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(processed.constData());
            const int numSamples = processed.size() / static_cast<int>(sizeof(int16_t));
            double sum = 0.0;
            for (int i = 0; i < numSamples; ++i) {
                sum += static_cast<double>(samples[i]) * samples[i];
            }
            const double rms = (numSamples > 0) ? std::sqrt(sum / numSamples) : 0.0;
            const double db = (rms > 0) ? 20.0 * std::log10(rms / 32768.0) : -100.0;
            qDebug().noquote()
                << QString("Audio: %1 B, RMS %2 (%3 dB), gain %4, speech %5")
                       .arg(processed.size())
                       .arg(rms, 0, 'f', 0)
                       .arg(db, 0, 'f', 1)
                       .arg(m_agc ? m_agc->currentGain() : 1.0f, 0, 'f', 2)
                       .arg(m_agc && m_agc->isSpeechDetected() ? QStringLiteral("YES")
                                                               : QStringLiteral("NO"));
        }

        // VAD в sherpa-onnx требует ровно 16 кГц моно int16 и сам не ресемплит
        if (m_mode != Mode::Off && m_pipeline) {
            m_pipeline->processAudio(processed, sampleRate);
        }
    }
}

void ApplicationController::onAudioError(const QString& message)
{
    qCritical() << "Audio error:" << message;
    emit errorOccurred(message);
}

// ---------------------------------------------------------------------------
// Результат распознавания
// ---------------------------------------------------------------------------

void ApplicationController::onRawTextRecognized(const QString& raw)
{
    // Команда ищется по «сырому» тексту: TextPostProcessor съедает слова
    // «точка», «новая строка», «пробел», превращая их в знаки, и команда
    // из готового текста уже не соберётся.
    if (!m_commands) {
        return;
    }
    if (const auto cmd = m_commands->parse(raw); cmd.has_value()) {
        m_skipNextText = true;
        executeCommand(*cmd);
    }
}

void ApplicationController::onTextReady(const QString& text)
{
    emit textRecognized(text);

    if (m_skipNextText) {
        m_skipNextText = false;   // фраза была командой — вставлять нечего
        return;
    }

    switch (m_mode) {
    case Mode::Dictation:
        injectText(text);
        break;

    case Mode::Spellcheck:
        // TODO: AspellChecker — прогнать текст и предложить замены
        qInfo().noquote() << QStringLiteral("Spellcheck (не реализовано): %1").arg(text);
        break;

    case Mode::Edit:
        // В режиме правки текст не вставляется: работают только команды
        break;

    case Mode::Off:
    case Mode::Error:
        break;
    }
}

void ApplicationController::injectText(const QString& text)
{
    if (!m_injector || !m_injector->isAvailable()) {
        emit errorOccurred(QStringLiteral("Некуда вставлять текст: инжектор недоступен"));
        return;
    }
    if (!m_injector->typeText(text)) {
        emit errorOccurred(QStringLiteral("Не удалось вставить текст"));
    }
}

void ApplicationController::executeCommand(const Command& cmd)
{
    const QString desc = commandDescription(cmd);

    if (cmd.type == Command::Type::SetMode) {
        emit commandExecuted(desc);
        setMode(cmd.mode);
        return;
    }

    // Правка текста в режиме диктовки по умолчанию запрещена: иначе продиктованное
    // «удали слово» съедало бы само себя. Включается [commands] editing_in_dictation=true.
    if (m_mode == Mode::Dictation && !m_editCmdsInDictation) {
        qInfo().noquote()
            << QStringLiteral("Команда «%1» пропущена: режим диктовки "
                              "(commands/editing_in_dictation=false)").arg(desc);
        return;
    }

    if (!m_injector || !m_injector->isAvailable()) {
        emit errorOccurred(QStringLiteral("Инжектор недоступен, команда «%1» не выполнена")
                               .arg(desc));
        return;
    }

    switch (cmd.type) {
    case Command::Type::DeleteWord:
        m_injector->sendKey(QStringLiteral("ctrl+BackSpace"));
        break;
    case Command::Type::DeleteLine:
        m_injector->sendKey(QStringLiteral("shift+Home"));
        m_injector->sendKey(QStringLiteral("BackSpace"));
        break;
    case Command::Type::NewLine:
        m_injector->sendKey(QStringLiteral("Return"));
        break;
    case Command::Type::Space:
        m_injector->sendKey(QStringLiteral("space"));
        break;
    case Command::Type::Punctuation:
        m_injector->typeText(cmd.argument);
        break;
    case Command::Type::SetMode:
    case Command::Type::Unknown:
        break;
    }

    emit commandExecuted(desc);
}
