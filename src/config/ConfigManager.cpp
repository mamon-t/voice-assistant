#include "ConfigManager.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString defaultConfigFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QStringLiteral("/voice-assistant/settings.ini");
}

}  // namespace

ConfigManager::ConfigManager()
    : m_settings(defaultConfigFile(), QSettings::IniFormat)
{
}

ConfigManager::ConfigManager(const QString& iniPath)
    : m_settings(iniPath, QSettings::IniFormat)
{
}

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------

QString ConfigManager::configDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QStringLiteral("/voice-assistant");
}

QString ConfigManager::settingsPath() const
{
    return m_settings.fileName();
}

QString ConfigManager::modelsPath() const
{
    return QDir::home().filePath(QStringLiteral(".voice_models"));
}

QString ConfigManager::resolvePath(const QString& path) const
{
    QString p = path.trimmed();
    if (p.isEmpty()) {
        return p;
    }
    if (p == QLatin1String("~")) {
        p = QDir::homePath();
    } else if (p.startsWith(QLatin1String("~/"))) {
        p = QDir::homePath() + p.mid(1);
    }
    if (QDir::isAbsolutePath(p)) {
        return QDir::cleanPath(p);
    }
    return QDir::cleanPath(modelsPath() + QLatin1Char('/') + p);
}

QString ConfigManager::sileroVadPath() const
{
    return resolvePath(m_settings.value(QStringLiteral("vad/model"),
                                        QStringLiteral("silero_vad.onnx")).toString());
}

QString ConfigManager::commandsHotwordsPath() const
{
    return configDir() + QStringLiteral("/commands_hotwords.txt");
}

QString ConfigManager::userHotwordsPath() const
{
    return configDir() + QStringLiteral("/user_hotwords.txt");
}

QStringList ConfigManager::loadHotwords(const QString& filePath) const
{
    QStringList words;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return words;
    }
    QTextStream in(&f);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            words << line;
        }
    }
    return words;
}

void ConfigManager::saveHotwords(const QString& filePath, const QStringList& words) const
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return;
    }
    QTextStream out(&f);
    out.setCodec("UTF-8");
    for (const QString& w : words) {
        const QString t = w.trimmed();
        if (!t.isEmpty()) {
            out << t << '\n';
        }
    }
}

// ---------------------------------------------------------------------------
// VAD
// ---------------------------------------------------------------------------

float ConfigManager::vadThreshold() const
{
    return m_settings.value(QStringLiteral("vad/threshold"), 0.5f).toFloat();
}

void ConfigManager::setVadThreshold(float threshold)
{
    m_settings.setValue(QStringLiteral("vad/threshold"), threshold);
}

float ConfigManager::vadMinSilenceDuration() const
{
    return m_settings.value(QStringLiteral("vad/min_silence_duration"), 0.5f).toFloat();
}

float ConfigManager::vadMinSpeechDuration() const
{
    return m_settings.value(QStringLiteral("vad/min_speech_duration"), 0.25f).toFloat();
}

int ConfigManager::vadWindowSize() const
{
    return m_settings.value(QStringLiteral("vad/window_size"), 512).toInt();
}

int ConfigManager::vadNumThreads() const
{
    return m_settings.value(QStringLiteral("vad/num_threads"), 1).toInt();
}

float ConfigManager::vadBufferSeconds() const
{
    return m_settings.value(QStringLiteral("vad/buffer_seconds"), 60.0f).toFloat();
}

// ---------------------------------------------------------------------------
// ASR-профили
// ---------------------------------------------------------------------------

QStringList ConfigManager::asrProfileNames() const
{
    // ВНИМАНИЕ: QSettings в IniFormat для значений со запятыми возвращает QVariant
    // со QStringList внутри, а .toString() на таком варианте даёт ПУСТУЮ строку.
    // Поэтому читаем и список, и строку.
    const QVariant raw = m_settings.value(QStringLiteral("asr/profiles"));
    QStringList parts = raw.toStringList();
    if (parts.isEmpty()) {
        parts = raw.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    }

    QStringList names;
    for (const QString& p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty() && !names.contains(t)) {
            names << t;
        }
    }
    return names;
}

QString ConfigManager::activeAsrProfileName() const
{
    QString name = m_settings.value(QStringLiteral("asr/active")).toString().trimmed();
    if (name.isEmpty()) {
        const QStringList names = asrProfileNames();
        if (!names.isEmpty()) {
            name = names.first();
        }
    }
    return name;
}

void ConfigManager::setActiveAsrProfileName(const QString& name)
{
    m_settings.setValue(QStringLiteral("asr/active"), name.trimmed());
}

bool ConfigManager::hasAsrProfile(const QString& name) const
{
    return m_settings.childGroups().contains(QStringLiteral("asr_%1").arg(name.trimmed()));
}

int ConfigManager::asrNumThreads() const
{
    return m_settings.value(QStringLiteral("asr/num_threads"), 2).toInt();
}

void ConfigManager::setAsrNumThreads(int threads)
{
    m_settings.setValue(QStringLiteral("asr/num_threads"), threads);
}

float ConfigManager::hotwordsScore() const
{
    return m_settings.value(QStringLiteral("asr/hotwords_score"), 2.0f).toFloat();
}

float ConfigManager::commandsHotwordsScore() const
{
    // 2.0 — замеренный минимум с запасом: при 1.5 и ниже модель начинает
    // терять служебные слова (см. docs/hotwords-and-punctuation.md)
    return m_settings.value(QStringLiteral("asr/commands_hotwords_score"), 2.0f).toFloat();
}

float ConfigManager::punctuationHotwordsScore() const
{
    // Замер на живой записи: 1.0/1.25/1.5 — «точка» теряется, 1.75 и 2.0 — нет.
    // Фантомных вставок «точки» на речи без диктантных слов при 2.0 не замечено.
    return m_settings.value(QStringLiteral("asr/punctuation_hotwords_score"), 2.0f).toFloat();
}

void ConfigManager::setHotwordsScore(float score)
{
    m_settings.setValue(QStringLiteral("asr/hotwords_score"), score);
}

AsrProfile ConfigManager::asrProfile(const QString& name) const
{
    AsrProfile p;
    p.name = name.trimmed();
    if (p.name.isEmpty()) {
        return p;
    }

    // beginGroup() не const, поэтому работаем полными ключами "asr_<имя>/<ключ>"
    const QString g = QStringLiteral("asr_%1/").arg(p.name);
    const auto v = [this, &g](const char* key, const QVariant& def = QVariant()) {
        return m_settings.value(g + QLatin1String(key), def);
    };

    p.engine = AsrProfile::engineFromString(v("engine").toString());

    p.encoderPath  = resolvePath(v("encoder").toString());
    p.decoderPath  = resolvePath(v("decoder").toString());
    p.joinerPath   = resolvePath(v("joiner").toString());
    p.ctcModelPath = resolvePath(v("model").toString());
    p.tokensPath   = resolvePath(v("tokens").toString());
    p.bpeVocabPath = resolvePath(v("bpe_vocab").toString());

    p.modelingUnit = v("modeling_unit").toString().trimmed();
    if (!p.bpeVocabPath.isEmpty() && p.modelingUnit.isEmpty()) {
        p.modelingUnit = QStringLiteral("bpe");
    }

    p.language = v("language", QStringLiteral("ru")).toString().trimmed();

    // Дефолт метода декодирования зависит от движка: hotwords работают только
    // в modified_beam_search, а для CTC в sherpa-onnx возможен лишь greedy_search.
    const QString defDecoding = (p.engine == AsrProfile::Engine::Transducer)
        ? QStringLiteral("modified_beam_search")
        : QStringLiteral("greedy_search");
    p.decodingMethod = v("decoding_method", defDecoding).toString().trimmed();

    p.maxActivePaths = v("max_active_paths", 4).toInt();
    p.numThreads     = v("num_threads", asrNumThreads()).toInt();
    p.hotwordsScore  = v("hotwords_score", hotwordsScore()).toFloat();
    p.debug          = v("debug", false).toBool();

    return p;
}

AsrProfile ConfigManager::activeAsrProfile() const
{
    return asrProfile(activeAsrProfileName());
}

// ---------------------------------------------------------------------------
// Постобработка текста
// ---------------------------------------------------------------------------

bool ConfigManager::autoPunctuate() const
{
    return m_settings.value(QStringLiteral("text/auto_punctuate"), true).toBool();
}

bool ConfigManager::voicePunctuation() const
{
    return m_settings.value(QStringLiteral("text/voice_punctuation"), true).toBool();
}

bool ConfigManager::capitalizeSentences() const
{
    // auto_punctuate — общий выключатель (обратная совместимость),
    // capitalize / add_final_dot позволяют оставить заглавные, но убрать точку.
    if (!autoPunctuate()) {
        return false;
    }
    return m_settings.value(QStringLiteral("text/capitalize"), true).toBool();
}

bool ConfigManager::addFinalDot() const
{
    if (!autoPunctuate()) {
        return false;
    }
    return m_settings.value(QStringLiteral("text/add_final_dot"), true).toBool();
}

// ---------------------------------------------------------------------------
// Вывод текста, команды, отладка
// ---------------------------------------------------------------------------

QString ConfigManager::injectorMethod() const
{
    return m_settings.value(QStringLiteral("output/method"), QStringLiteral("auto"))
        .toString().trimmed().toLower();
}

int ConfigManager::typingDelayMs() const
{
    return m_settings.value(QStringLiteral("output/typing_delay_ms"), 12).toInt();
}

bool ConfigManager::spaceBetweenSegments() const
{
    return m_settings.value(QStringLiteral("output/space_between_segments"), true).toBool();
}

bool ConfigManager::preserveClipboard() const
{
    return m_settings.value(QStringLiteral("output/preserve_clipboard"), true).toBool();
}

int ConfigManager::clipboardRestoreMs() const
{
    return m_settings.value(QStringLiteral("output/clipboard_restore_ms"), 1000).toInt();
}

bool ConfigManager::editingCommandsInDictation() const
{
    return m_settings.value(QStringLiteral("commands/editing_in_dictation"), false).toBool();
}

QString ConfigManager::customCommandsPath() const
{
    const QString v = m_settings.value(QStringLiteral("commands/file")).toString().trimmed();
    if (v.isEmpty()) {
        return configDir() + QStringLiteral("/commands.txt");
    }
    return resolvePath(v);
}

bool ConfigManager::spellcheckEnabled() const
{
    return m_settings.value(QStringLiteral("spellcheck/enabled"), true).toBool();
}

QString ConfigManager::spellcheckLang() const
{
    return m_settings.value(QStringLiteral("spellcheck/lang"), QStringLiteral("ru_RU"))
        .toString().trimmed();
}

QString ConfigManager::spellcheckDictionaryDir() const
{
    return resolvePath(m_settings.value(QStringLiteral("spellcheck/dictionary_dir")).toString());
}

int ConfigManager::spellcheckMaxSuggestions() const
{
    return m_settings.value(QStringLiteral("spellcheck/max_suggestions"), 5).toInt();
}

QString ConfigManager::micCheckDir() const
{
    const QString v = m_settings.value(QStringLiteral("audio/mic_check_dir")).toString().trimmed();
    if (!v.isEmpty()) {
        return resolvePath(v);
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/recordings");
}

QString ConfigManager::micCheckSource() const
{
    const QString v = m_settings.value(QStringLiteral("audio/mic_check_source"),
                                       QStringLiteral("agc")).toString().trimmed().toLower();
    return (v == QLatin1String("raw")) ? QStringLiteral("raw") : QStringLiteral("agc");
}

QString ConfigManager::micCheckPrefix() const
{
    const QString v = m_settings.value(QStringLiteral("audio/mic_check_prefix"),
                                       QStringLiteral("mic-check")).toString().trimmed();
    return v.isEmpty() ? QStringLiteral("mic-check") : v;
}

QString ConfigManager::hotkeyBackend() const
{
    return m_settings.value(QStringLiteral("hotkey/backend"), QStringLiteral("evdev"))
        .toString().trimmed().toLower();
}

QString ConfigManager::hotkeyKey() const
{
    return m_settings.value(QStringLiteral("hotkey/key"), QStringLiteral("KEY_F8"))
        .toString().trimmed();
}

bool ConfigManager::hotkeyGrab() const
{
    // По умолчанию false и это принципиально: EVIOCGRAB забирает устройство
    // эксклюзивно, и X-сервер перестаёт получать с него ВСЕ клавиши.
    // Включать имеет смысл только для выделенной педали или второй клавиатуры.
    return m_settings.value(QStringLiteral("hotkey/grab"), false).toBool();
}

QStringList ConfigManager::hotkeyGrabDevices() const
{
    // ВАЖНО: именно toStringList(). QSettings возвращает QStringList для
    // значений с запятыми, а toString() для такого значения даёт пустую строку
    // (грабля №9 в development-log.md).
    QStringList out;
    const QVariant v = m_settings.value(QStringLiteral("hotkey/grab_devices"));
    // userType() есть и в Qt5, и в Qt6 (в отличие от typeId()/type())
    if (v.userType() == QMetaType::QStringList) {
        out = v.toStringList();
    } else {
        out = v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    for (QString& s : out) {
        s = s.trimmed();
    }
    out.removeAll(QString());
    return out;
}

QString ConfigManager::hotkeyMode() const
{
    const QString v = m_settings.value(QStringLiteral("hotkey/mode"),
                                       QStringLiteral("push_to_talk")).toString().trimmed().toLower();
    return (v == QLatin1String("toggle")) ? QStringLiteral("toggle")
                                          : QStringLiteral("push_to_talk");
}

bool ConfigManager::audioDebugLog() const
{
    return m_settings.value(QStringLiteral("audio/debug_log"), false).toBool();
}

// ---------------------------------------------------------------------------
// Вывод текста: привязка к окну
// ---------------------------------------------------------------------------

bool ConfigManager::pinWindow() const
{
    return m_settings.value(QStringLiteral("output/pin_window"), false).toBool();
}

QString ConfigManager::pinMode() const
{
    // По умолчанию activate: это единственный способ, который действительно
    // печатает в любом приложении. Тихий откат к sendevent мы уже проходили —
    // «лог виден, а в редакторе ничего».
    return m_settings.value(QStringLiteral("output/pin_mode"),
                            QStringLiteral("activate")).toString();
}

int ConfigManager::pinActivateMs() const
{
    const int v = m_settings.value(QStringLiteral("output/pin_activate_ms"), 80).toInt();
    return (v < 0) ? 0 : v;
}

bool ConfigManager::pinRestoreFocus() const
{
    return m_settings.value(QStringLiteral("output/pin_restore_focus"), true).toBool();
}

QStringList ConfigManager::ownWindowClasses() const
{
    // Пусто быть не должно: тогда привязка к собственному меню трея снова
    // проглотит текст. Поэтому список по умолчанию задаём здесь, а не в .ini,
    // и возвращаем его же, если значение отсутствует или оказалось пустым.
    static const QStringList kDefault = {
        QStringLiteral("voice-assistant"),
        QStringLiteral("VoiceAssistant"),
        QStringLiteral("voiceassistant"),
    };

    // ВАЖНО: именно toStringList(). QSettings возвращает QStringList для
    // значений с запятыми, а toString() для такого значения даёт ПУСТУЮ строку
    // (грабля №9 в development-log.md — тест configPinWindowReadsIni это поймал).
    const QVariant v = m_settings.value(QStringLiteral("output/own_window_class"));
    if (!v.isValid()) {
        return kDefault;
    }
    QStringList out;
    if (v.userType() == QMetaType::QStringList) {
        out = v.toStringList();
    } else {
        out = v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    for (QString& s : out) {
        s = s.trimmed();
    }
    out.removeAll(QString());
    return out.isEmpty() ? kDefault : out;
}

// ---------------------------------------------------------------------------
// Заметки (цель вывода Notes)
// ---------------------------------------------------------------------------

bool ConfigManager::notesEnabled() const
{
    return m_settings.value(QStringLiteral("notes/enabled"), true).toBool();
}

QString ConfigManager::notesDir() const
{
    const QString v = m_settings.value(QStringLiteral("notes/dir")).toString().trimmed();
    if (v.isEmpty()) {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + QStringLiteral("/notes");
    }
    // resolvePath() считает относительные пути от каталога моделей — для заметок
    // это было бы сюрпризом, поэтому раскрываем "~" и считаем относительный
    // путь от домашнего каталога.
    if (v == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (v.startsWith(QLatin1String("~/"))) {
        return QDir::cleanPath(QDir::homePath() + v.mid(1));
    }
    if (QDir::isAbsolutePath(v)) {
        return QDir::cleanPath(v);
    }
    return QDir::cleanPath(QDir::homePath() + QLatin1Char('/') + v);
}

QString ConfigManager::notesFile() const
{
    const QString v = m_settings.value(QStringLiteral("notes/file")).toString().trimmed();
    if (v.isEmpty()) {
        return QString();
    }
    if (v == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (v.startsWith(QLatin1String("~/"))) {
        return QDir::cleanPath(QDir::homePath() + v.mid(1));
    }
    if (QDir::isAbsolutePath(v)) {
        return QDir::cleanPath(v);
    }
    return QDir::cleanPath(QDir::homePath() + QLatin1Char('/') + v);
}

QString ConfigManager::notesTimestampFormat() const
{
    return m_settings.value(QStringLiteral("notes/timestamp"), QStringLiteral("HH:mm:ss"))
        .toString();
}

bool ConfigManager::notesMarkdown() const
{
    return m_settings.value(QStringLiteral("notes/markdown"), true).toBool();
}

bool ConfigManager::notesDayHeader() const
{
    return m_settings.value(QStringLiteral("notes/day_header"), true).toBool();
}

bool ConfigManager::notesVoiceCommands() const
{
    return m_settings.value(QStringLiteral("notes/voice_commands"), true).toBool();
}

QString ConfigManager::notesStartTarget() const
{
    return m_settings.value(QStringLiteral("notes/start_target"), QStringLiteral("focus"))
        .toString().trimmed().toLower();
}

// ---------------------------------------------------------------------------
// Глушитель микрофона на время печати
// ---------------------------------------------------------------------------

int ConfigManager::typingGuardMs() const
{
    const int v = m_settings.value(QStringLiteral("audio/typing_guard_ms"), 0).toInt();
    return (v < 0) ? 0 : v;
}
