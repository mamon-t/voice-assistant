#include "ApplicationController.h"

#include "audio/Agc.h"
#include "audio/QtAudioCapture.h"
#include "audio/WavWriter.h"
#include "input/EvdevHotkeyListener.h"
#include "input/IHotkeyListener.h"
#include "commands/CommandDictionary.h"
#include "commands/CommandParser.h"
#include "config/ConfigManager.h"
#include "config/HotwordsManager.h"
#include "core/VoicePipeline.h"
#include "output/FileInjector.h"
#include "output/XdotoolInjector.h"
#include "spellcheck/HunspellChecker.h"
#include "text/TextPostProcessor.h"
#include "spellcheck/ISpellChecker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <cmath>
#include <cstdint>

namespace {

QString commandDescription(const Command& cmd)
{
    switch (cmd.type) {
    case Command::Type::SetMode:     return QStringLiteral("режим: %1").arg(modeToString(cmd.mode));
    case Command::Type::SetTarget:   return QStringLiteral("куда писать: %1")
                                               .arg(outputTargetTitle(cmd.target));
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

    // Слово «заметка» встречается в обычной речи. Если оно мешает, голосовое
    // переключение цели выключается ([notes] voice_commands=false), а цель
    // остаётся доступной из трея, по D-Bus и хоткеем.
    if (!m_config->notesVoiceCommands()) {
        int removed = 0;
        const QStringList phrases = CommandDictionary::targetCommandPhrases();
        for (const QString& phrase : phrases) {
            if (m_commands->dictionary()->removeCommand(phrase)) {
                ++removed;
            }
        }
        qInfo().noquote()
            << QStringLiteral("Голосовые команды заметок выключены ([notes] "
                              "voice_commands=false), убрано фраз: %1").arg(removed);
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
    injector->setPreserveClipboard(m_config->preserveClipboard());
    injector->setClipboardRestoreMs(m_config->clipboardRestoreMs());

    // Как доставлять текст в привязанное окно. activate (по умолчанию) —
    // окно активируется и текст печатается настоящими событиями: работает
    // везде, но на ~50–150 мс забирает фокус. sendevent — прежнее поведение
    // (xdotool --window): фокус не трогается, зато часть приложений такой
    // ввод отбрасывает, и xdotool всё равно возвращает 0.
    injector->setPinMode(stringToPinMode(m_config->pinMode()));
    injector->setPinActivateDelayMs(m_config->pinActivateMs());
    injector->setPinRestoreFocus(m_config->pinRestoreFocus());
    injector->setOwnWindowClasses(m_config->ownWindowClasses());

    if (!injector->initialize() || !injector->isAvailable()) {
        reportError(QStringLiteral(
            "Ввод текста недоступен: нужен xdotool (X11) и, желательно, xclip. "
            "Для Wayland/TTY требуется ydotool."));
    }
    m_injector = std::move(injector);

    // 4b. Заметки, привязка к окну, глушитель печати ----------------------------
    m_clock.start();
    setupNotes();

    m_pinWindow = m_config->pinWindow();
    m_typingGuard.setGuardMs(m_config->typingGuardMs());
    if (m_typingGuard.isEnabled()) {
        qInfo().noquote()
            << QStringLiteral("Глушитель печати: %1 мс (аудио не уходит в ASR, пока "
                              "идёт набор текста)").arg(m_typingGuard.guardMs());
        if (!isHotkeyActive() && m_config->hotkeyBackend() != QLatin1String("evdev")) {
            qWarning().noquote()
                << QStringLiteral("Глушитель печати не сработает: нужны события клавиатуры, "
                                  "а [hotkey] backend != evdev");
        }
    }

    // 5. Проверка правописания ---------------------------------------------------
    // Не критично для диктовки: если словаря или библиотеки нет, сообщаем и живём дальше.
    if (m_config->spellcheckEnabled()) {
        HunspellChecker::Options sopt;
        sopt.lang           = m_config->spellcheckLang();
        sopt.dictionaryDir  = m_config->spellcheckDictionaryDir();
        sopt.maxSuggestions = m_config->spellcheckMaxSuggestions();

        auto checker = std::make_unique<HunspellChecker>(sopt);
        if (checker->initialize()) {
            m_spellChecker = std::move(checker);
        } else {
            qWarning().noquote()
                << QStringLiteral("Проверка правописания недоступна: %1")
                       .arg(checker->lastError());
        }
    }

    // 6. Аудиоподсистема --------------------------------------------------------
    m_audioCapture = std::make_unique<QtAudioCapture>();
    if (!m_audioCapture->initialize()) {
        reportError(QStringLiteral("Не удалось инициализировать захват звука"));
        m_mode = Mode::Error;
        emit modeChanged(m_mode);
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

    m_micCheckSource = m_config->micCheckSource();
    m_hotkeyMode     = m_config->hotkeyMode();

    // 7. Речевой пайплайн (VAD -> ASR -> пунктуация) -----------------------------
    if (!buildPipeline()) {
        // Без ASR приложение работать не может. Не делаем вид, что всё в порядке:
        // переводимся в Mode::Error (трей покажет error.svg) и сообщаем наружу.
        m_mode = Mode::Error;
        emit modeChanged(m_mode);
        return;
    }

    // 8. Глобальный хоткей (не критично: без него остаются трей и D-Bus)
    setupHotkey();
}

void ApplicationController::reportError(const QString& message)
{
    m_lastError = message;
    qCritical().noquote() << message;
    emit errorOccurred(message);
}

bool ApplicationController::isReady() const
{
    return static_cast<bool>(m_pipeline);
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
        m_pipeline.reset();
        reportError(QStringLiteral("Речевой тракт не запущен: %1. "
                                   "Диагностика: ./src/voice-assistant --check")
                        .arg(err));
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
    // Пользовательские подсказки идут с общим скором ([asr] hotwords_score),
    // фразы команд — со своим, более мягким: их много (26+), и сильный буст
    // начинает протаскивать служебные слова в обычную речь.
    QStringList hw = m_hotwords ? m_hotwords->allHotwords() : QStringList();
    if (m_commands && m_commands->dictionary()) {
        const QString suffix = QStringLiteral(" :%1")
                                   .arg(m_config ? m_config->commandsHotwordsScore() : 1.5f, 0, 'f', 2);
        const QStringList phrases = m_commands->dictionary()->phrases();
        for (const QString& p : phrases) {
            if (!hw.contains(p)) {
                hw << p + suffix;
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
    if (!m_pipeline) {
        reportError(QStringLiteral(
            "Запись не начата: речевой тракт не инициализирован. "
            "Проверьте модели — ./src/voice-assistant --check"));
        return;
    }
    if (m_mode == Mode::Off) {
        if (m_agc)      m_agc->reset();
        if (m_pipeline) m_pipeline->reset();
        m_audioBuffer.clear();
        m_skipNextText = false;
        m_lastInjected.clear();
        m_typingGuard.reset();
        captureTargetWindow();

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
        if (m_audioCapture && !isMicChecking()) {
            m_audioCapture->stop();   // запись микрофона держит захват сама
        }
        m_audioBuffer.clear();
        m_mode = Mode::Off;

        // Без этой строки непонятно, не съел ли глушитель печати саму речь:
        // если секунд много, [audio] typing_guard_ms стоит уменьшить.
        if (m_typingGuard.droppedChunks() > 0) {
            qInfo().noquote()
                << QStringLiteral("Глушитель печати: выброшено %1 кусков аудио (%2 с) — "
                                  "столько речи не дошло до ASR")
                       .arg(m_typingGuard.droppedChunks())
                       .arg(m_typingGuard.droppedSamples() / 16000.0, 0, 'f', 2);
        }
        qDebug() << "Recording stopped";
        emit modeChanged(m_mode);
    }
}

void ApplicationController::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }
    if (mode != Mode::Off && !m_pipeline) {
        reportError(QStringLiteral(
            "Режим %1 не включён: речевой тракт не инициализирован. "
            "Проверьте модели — ./src/voice-assistant --check")
                        .arg(modeToString(mode)));
        return;
    }

    if (m_mode == Mode::Off && mode != Mode::Off) {
        if (m_agc)      m_agc->reset();
        if (m_pipeline) m_pipeline->reset();
        m_audioBuffer.clear();
        m_skipNextText = false;
        m_lastInjected.clear();
        m_typingGuard.reset();
        captureTargetWindow();
        if (m_audioCapture) m_audioCapture->start();
    } else if (mode == Mode::Off) {
        if (m_pipeline) m_pipeline->flush();
        if (m_audioCapture && !isMicChecking()) {
            m_audioCapture->stop();
        }
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
        reportError(QStringLiteral("Нет профиля ASR: %1").arg(profileName));
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

        // Запись тракта для диагностики (работает в любом режиме).
        // Пишется ВСЕГДА, в том числе когда глушитель печати выбрасывает аудио:
        // снимок микрофона обязан показывать весь тракт, иначе его нельзя
        // сравнивать с тем, что слышит ASR.
        if (m_wavWriter && m_wavWriter->isOpen()) {
            m_wavWriter->write(m_micCheckSource == QLatin1String("raw") ? chunk : processed);
        }

        // VAD в sherpa-onnx требует ровно 16 кГц моно int16 и сам не ресемплит
        if (m_mode != Mode::Off && m_pipeline) {
            if (m_typingGuard.blocked(m_clock.elapsed())) {
                // Идёт печать: стук клавиш не должен превращаться в текст,
                // а продиктованное — вклиниваться в напечатанное.
                m_typingGuard.countDropped(processed.size() / 2);
            } else {
                m_pipeline->processAudio(processed, sampleRate);
            }
        }
    }
}

void ApplicationController::onAudioError(const QString& message)
{
    qCritical() << "Audio error:" << message;
    emit errorOccurred(message);
}

// ---------------------------------------------------------------------------
// Проверка микрофона: запись тракта в WAV
// ---------------------------------------------------------------------------

bool ApplicationController::isMicChecking() const
{
    return m_wavWriter && m_wavWriter->isOpen();
}

QString ApplicationController::micCheckFile() const
{
    return isMicChecking() ? m_wavWriter->filePath() : QString();
}

bool ApplicationController::startMicCheck(QString* outFile)
{
    if (isMicChecking()) {
        if (outFile) {
            *outFile = m_wavWriter->filePath();
        }
        return true;
    }
    if (!m_audioCapture) {
        reportError(QStringLiteral("Запись микрофона невозможна: захват звука не создан"));
        return false;
    }

    const QString dir  = m_config ? m_config->micCheckDir()
                                  : QDir::homePath() + QStringLiteral("/voice-assistant-recordings");
    const QString name = WavWriter::defaultFileName(
        m_config ? m_config->micCheckPrefix() : QStringLiteral("mic-check"));
    const QString path = QDir(dir).filePath(name);

    m_wavWriter = std::make_unique<WavWriter>();
    // Захват отдаёт 16 кГц моно int16 — ровно то, что нужно WAV'у и ASR
    if (!m_wavWriter->open(path, 16000, 1, 16)) {
        reportError(m_wavWriter->lastError());
        m_wavWriter.reset();
        return false;
    }

    if (m_agc) {
        m_agc->reset();
    }
    m_micCheckOwnsCapture = !m_audioCapture->isRunning();
    if (m_micCheckOwnsCapture) {
        m_audioCapture->start();   // режим не меняем: это диагностика, не диктовка
    }

    if (outFile) {
        *outFile = path;
    }
    qInfo().noquote() << QStringLiteral("Запись микрофона начата: %1 (источник=%2)")
                             .arg(path, m_micCheckSource);
    emit micCheckStarted(path);
    return true;
}

void ApplicationController::stopMicCheck()
{
    if (!isMicChecking()) {
        return;
    }

    const QString path   = m_wavWriter->filePath();
    const double  secs   = m_wavWriter->seconds();
    m_wavWriter->close();
    m_wavWriter.reset();

    if (m_micCheckOwnsCapture && m_audioCapture && m_mode == Mode::Off) {
        m_audioCapture->stop();
    }
    m_micCheckOwnsCapture = false;

    qInfo().noquote() << QStringLiteral("Запись сохранена: %1 (%2 с)")
                             .arg(path).arg(secs, 0, 'f', 2);
    emit micCheckFinished(path, secs);
}

// ---------------------------------------------------------------------------
// Глобальный хоткей
// ---------------------------------------------------------------------------

void ApplicationController::setupHotkey()
{
    const QString backend = m_config ? m_config->hotkeyBackend() : QStringLiteral("off");
    if (backend == QLatin1String("off") || backend.isEmpty()) {
        qInfo().noquote() << QStringLiteral("Хоткей: выключен ([hotkey] backend=off)");
        return;
    }
    if (backend != QLatin1String("evdev")) {
        reportError(QStringLiteral("Неизвестный [hotkey] backend='%1' (доступны: evdev, off)")
                        .arg(backend));
        return;
    }

    EvdevHotkeyListener::Options opt;
    opt.key         = m_config->hotkeyKey();
    opt.grab        = m_config->hotkeyGrab();
    opt.grabDevices = m_config->hotkeyGrabDevices();

    auto listener = std::make_unique<EvdevHotkeyListener>(opt);
    connect(listener.get(), &IHotkeyListener::errorOccurred,
            this, &ApplicationController::errorOccurred);
    connect(listener.get(), &IHotkeyListener::pressed,  this, &ApplicationController::onHotkeyPressed);
    connect(listener.get(), &IHotkeyListener::released, this, &ApplicationController::onHotkeyReleased);
    // Печать пользователя глушит микрофон (см. audio/TypingGuard.h)
    connect(listener.get(), &IHotkeyListener::keyActivity,
            this, &ApplicationController::onKeyActivity);

    QString err;
    if (!listener->start(&err)) {
        // Не смертельно: трей и D-Bus продолжают работать
        reportError(QStringLiteral("Хоткей не запущен: %1").arg(err));
        return;
    }
    m_hotkey = std::move(listener);
}

bool ApplicationController::isHotkeyActive() const
{
    return m_hotkey && m_hotkey->isActive();
}

QString ApplicationController::hotkeyDescription() const
{
    if (!m_hotkey) {
        return QStringLiteral("выключен");
    }
    return QStringLiteral("%1, клавиша %2, режим %3")
        .arg(m_hotkey->backendName(), m_config->hotkeyKey(), m_hotkeyMode);
}

void ApplicationController::onHotkeyPressed()
{
    if (m_hotkeyMode == QLatin1String("toggle")) {
        if (m_mode == Mode::Off) {
            startRecording();
        } else {
            stopRecording();
        }
        return;
    }
    // push-to-talk: держим клавишу — идёт запись
    if (m_mode == Mode::Off) {
        startRecording();
    }
}

void ApplicationController::onHotkeyReleased()
{
    if (m_hotkeyMode != QLatin1String("toggle") && m_mode != Mode::Off) {
        stopRecording();   // внутри flush() — хвост фразы не теряется
    }
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
        // Диктуем как обычно и тут же проверяем: текст вставляется, а о найденных
        // ошибках сообщаем сигналом (трей показывает их уведомлением).
        injectText(text);
        runSpellcheck(text);
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
    // Цель «заметки»: файл. Ни фокус окна, ни клавиатура, ни буфер обмена
    // не участвуют, поэтому печатать руками можно параллельно с диктовкой.
    if (m_target == OutputTarget::Notes) {
        if (!m_notes || !m_notes->isAvailable()) {
            reportError(QStringLiteral("Заметки недоступны: файл не открыт "
                                       "([notes] enabled=false или нет прав на каталог)"));
            return;
        }
        if (!m_notes->typeText(text)) {
            reportError(QStringLiteral("Не удалось записать заметку: %1")
                            .arg(m_notes->lastError()));
            return;
        }
        qInfo().noquote() << QStringLiteral("Заметка -> %1").arg(m_notes->filePath());
        emit noteWritten(m_notes->filePath(), text);
        return;
    }

    if (!m_injector || !m_injector->isAvailable()) {
        reportError(QStringLiteral("Некуда вставлять текст: инжектор недоступен"));
        return;
    }

    // Привязанное окно могли закрыть, пока шла запись (или это было всплывающее
    // меню, которое уже исчезло). Тогда вставка ушла бы в никуда: снимаем
    // привязку и честно пишем в лог, что текст пойдёт в активное окно.
    auto* xdotool = dynamic_cast<XdotoolInjector*>(m_injector.get());
    if (xdotool && !m_pinnedWindow.isEmpty()
        && !XdotoolInjector::windowExists(m_pinnedWindow)) {
        qWarning().noquote()
            << QStringLiteral("pin_window: окно %1 закрыто, привязка снята — "
                              "текст пойдёт в активное окно").arg(m_pinnedWindow);
        m_pinnedWindow.clear();
        xdotool->clearPinnedWindow();
    }

    // VAD отдаёт речь отдельными фразами, каждая вставляется своим вызовом.
    // Без разделителя получалось "…двадцать лет назад.Сегодня вот…" — добавляем
    // пробел, если предыдущая вставка им не закончилась.
    QString toType = text;
    if (m_config->spaceBetweenSegments()
        && TextPostProcessor::needsLeadingSpace(m_lastInjected, toType)) {
        toType.prepend(QLatin1Char(' '));
    }

    if (!m_injector->typeText(toType)) {
        // «Не удалось вставить текст» без адреса ни о чём не говорит, поэтому
        // рядом всегда печатаем КУДА именно мы целились.
        reportError(QStringLiteral("Не удалось вставить текст (%1 символ) в %2%3")
                        .arg(toType.size())
                        .arg(m_pinnedWindow.isEmpty()
                                 ? QStringLiteral("активное окно")
                                 : QStringLiteral("окно ") + m_pinnedWindow,
                             xdotool ? QStringLiteral(": ") + xdotool->lastError() : QString()));
        return;
    }
    m_lastInjected = toType;
}

// ---------------------------------------------------------------------------
// Проверка правописания
// ---------------------------------------------------------------------------

bool ApplicationController::isSpellcheckAvailable() const
{
    return static_cast<bool>(m_spellChecker);
}

QStringList ApplicationController::checkText(const QString& text)
{
    return m_spellChecker ? m_spellChecker->check(text) : QStringList();
}

QStringList ApplicationController::suggestionsFor(const QString& word)
{
    return m_spellChecker ? m_spellChecker->suggest(word) : QStringList();
}

QString ApplicationController::spellcheckStatus() const
{
    if (!m_spellChecker) {
        return QStringLiteral("недоступна (нет hunspell или словаря)");
    }
    const auto* hun = dynamic_cast<const HunspellChecker*>(m_spellChecker.get());
    return hun ? QStringLiteral("hunspell, словарь %1").arg(hun->dictionaryPath())
               : QStringLiteral("доступна");
}

void ApplicationController::runSpellcheck(const QString& text)
{
    if (!m_spellChecker) {
        return;
    }

    const QStringList errors = m_spellChecker->check(text);
    if (errors.isEmpty()) {
        qInfo().noquote() << QStringLiteral("Орфография: ошибок нет");
        emit spellcheckFinished(text, errors);
        return;
    }

    QStringList report;
    report.reserve(errors.size());
    for (const QString& word : errors) {
        const QStringList sug = m_spellChecker->suggest(word);
        report << (sug.isEmpty() ? word
                                 : QStringLiteral("%1 -> %2").arg(word, sug.join(QStringLiteral(", "))));
    }

    qWarning().noquote()
        << QStringLiteral("Орфография: найдено %1 — %2")
               .arg(errors.size())
               .arg(report.join(QStringLiteral("; ")));

    emit spellcheckFinished(text, errors);
}

void ApplicationController::executeCommand(const Command& cmd)
{
    const QString desc = commandDescription(cmd);

    if (cmd.type == Command::Type::SetMode) {
        emit commandExecuted(desc);
        setMode(cmd.mode);
        return;
    }

    // Смена цели вывода — это маршрутизация, а не правка текста, поэтому она
    // обязана работать и в режиме диктовки при editing_in_dictation=false.
    // Иначе сказанное «заметка» пропало бы именно тогда, когда оно нужно.
    if (cmd.type == Command::Type::SetTarget) {
        emit commandExecuted(desc);
        setOutputTarget(cmd.target);
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

    // Команды правки идут в ту же цель, что и текст: для заметок это файл
    // («удали слово» сотрёт последнее слово последней заметки, «новая строка»
    // добавит пустую строку).
    ITextInjector* out = activeInjector();
    if (!out) {
        reportError(QStringLiteral("Инжектор недоступен, команда «%1» не выполнена")
                        .arg(desc));
        return;
    }

    switch (cmd.type) {
    case Command::Type::DeleteWord:
        out->sendKey(QStringLiteral("ctrl+BackSpace"));
        break;
    case Command::Type::DeleteLine:
        out->sendKey(QStringLiteral("shift+Home"));
        out->sendKey(QStringLiteral("BackSpace"));
        break;
    case Command::Type::NewLine:
        out->sendKey(QStringLiteral("Return"));
        break;
    case Command::Type::Space:
        out->sendKey(QStringLiteral("space"));
        break;
    case Command::Type::Punctuation:
        out->typeText(cmd.argument);
        break;
    case Command::Type::SetMode:
    case Command::Type::SetTarget:
    case Command::Type::Unknown:
        break;
    }

    emit commandExecuted(desc);
}

// ---------------------------------------------------------------------------
// Цель вывода: активное окно или файл заметок
// ---------------------------------------------------------------------------

void ApplicationController::setupNotes()
{
    if (!m_config->notesEnabled()) {
        qInfo().noquote() << QStringLiteral("Заметки выключены ([notes] enabled=false)");
        return;
    }

    FileInjector::Options nopt;
    nopt.dir            = m_config->notesDir();
    nopt.file           = m_config->notesFile();
    nopt.timestampFormat = m_config->notesTimestampFormat();
    nopt.markdown       = m_config->notesMarkdown();
    nopt.dayHeader      = m_config->notesDayHeader();

    auto notes = std::make_unique<FileInjector>(nopt);
    if (!notes->initialize()) {
        // Не смертельно: диктовка в окно продолжает работать.
        reportError(QStringLiteral("Заметки недоступны: %1").arg(notes->lastError()));
        return;
    }
    m_notes = std::move(notes);

    if (m_config->notesStartTarget() == QLatin1String("notes")) {
        m_target = OutputTarget::Notes;
        qInfo().noquote()
            << QStringLiteral("Цель вывода по умолчанию — файл заметок: %1")
                   .arg(m_notes->filePath());
    }
}

OutputTarget ApplicationController::outputTarget() const
{
    return m_target;
}

QString ApplicationController::outputTargetName() const
{
    return outputTargetToString(m_target);
}

bool ApplicationController::isNotesAvailable() const
{
    return m_notes && m_notes->isAvailable();
}

QString ApplicationController::notesFilePath() const
{
    return m_notes ? m_notes->filePath() : QString();
}

bool ApplicationController::setOutputTarget(OutputTarget target)
{
    if (target == OutputTarget::Notes && !isNotesAvailable()) {
        reportError(QStringLiteral("Заметки недоступны: включите [notes] enabled "
                                   "и проверьте права на каталог %1")
                        .arg(m_config ? m_config->notesDir() : QString()));
        return false;
    }
    if (m_target == target) {
        return true;
    }

    m_target = target;
    // Пробел между сегментами считается от последней вставки В ОКНО; для файла
    // каждая запись — своя строка, поэтому хвост сбрасываем.
    m_lastInjected.clear();

    qInfo().noquote()
        << QStringLiteral("Куда писать: %1%2")
               .arg(outputTargetTitle(m_target),
                    m_target == OutputTarget::Notes && m_notes
                        ? QStringLiteral(" (%1)").arg(m_notes->filePath())
                        : QString());
    emit outputTargetChanged(m_target);
    return true;
}

bool ApplicationController::toggleOutputTarget()
{
    return setOutputTarget(m_target == OutputTarget::Notes ? OutputTarget::Focus
                                                           : OutputTarget::Notes);
}

ITextInjector* ApplicationController::activeInjector() const
{
    if (m_target == OutputTarget::Notes) {
        return (m_notes && m_notes->isAvailable()) ? m_notes.get() : nullptr;
    }
    return (m_injector && m_injector->isAvailable()) ? m_injector.get() : nullptr;
}

// ---------------------------------------------------------------------------
// Привязка вставки к окну и глушитель печати
// ---------------------------------------------------------------------------

void ApplicationController::captureTargetWindow()
{
    if (!m_pinWindow) {
        return;
    }
    auto* xdotool = dynamic_cast<XdotoolInjector*>(m_injector.get());
    if (!xdotool) {
        return;
    }

    const QString wid = XdotoolInjector::activeWindowId();
    if (wid.isEmpty()) {
        qWarning().noquote()
            << QStringLiteral("pin_window: не удалось определить активное окно, "
                              "вставка пойдёт туда, где будет фокус");
        xdotool->clearPinnedWindow();
        m_pinnedWindow.clear();
        return;
    }

    // ЗАЩИТА ОТ САМОЙ ОБИДНОЙ ПОЛОМКИ. Запись часто начинают кликом по меню
    // трея, а в X11 всплывающее меню — это отдельное окно, и именно оно в этот
    // момент активно. Привязка к нему означала, что весь продиктованный текст
    // уезжает в меню, которое закрывается через мгновение вместе с текстом:
    // в логе при этом «успех», а в редакторе ничего. Окна помощника узнаём по
    // WM_CLASS ([output] own_window_class) и по PID собственного процесса.
    const QString cls = XdotoolInjector::windowClass(wid);
    const QStringList own = m_config ? m_config->ownWindowClasses() : QStringList();
    const qint64 pid = XdotoolInjector::windowPid(wid);
    const bool isSelf = (!cls.isEmpty() && own.contains(cls))
                        || (pid != 0 && pid == QCoreApplication::applicationPid());
    if (isSelf) {
        qWarning().noquote()
            << QStringLiteral("pin_window: активное окно %1 принадлежит самому помощнику "
                              "(WM_CLASS=%2, pid=%3) — привязка НЕ установлена, текст пойдёт "
                              "в окно, которое будет в фокусе на момент вставки. "
                              "Начинайте запись хоткеем (%4), а не кликом по меню трея.")
                   .arg(XdotoolInjector::describeWindow(wid),
                        cls.isEmpty() ? QStringLiteral("?") : cls)
                   .arg(pid)
                   .arg(m_config ? m_config->hotkeyKey() : QStringLiteral("хоткей"));
        xdotool->clearPinnedWindow();
        m_pinnedWindow.clear();
        return;
    }

    m_pinnedWindow = wid;
    xdotool->setPinnedWindow(wid);
}

void ApplicationController::onKeyActivity()
{
    if (m_typingGuard.isEnabled()) {
        m_typingGuard.noteKeyActivity(m_clock.elapsed());
    }
}
