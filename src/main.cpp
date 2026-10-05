// src/main.cpp
#include "core/ApplicationController.h"
#include "dbus/DBusInterface.h"
#include "ui/TrayIcon.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QThread>

#include "audio/QtAudioCapture.h"
#include "audio/AudioFileDecoder.h"
#include "audio/WavWriter.h"
#include "commands/CommandDictionary.h"
#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "core/VoicePipeline.h"
#include "input/EvdevHotkeyListener.h"
#include "models/ModelCatalog.h"
#include "models/ModelDownloader.h"
#include "output/FileInjector.h"
#include "output/XdotoolInjector.h"

#include <QElapsedTimer>
#include <QTimer>
#include "spellcheck/HunspellChecker.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <QFile>

namespace {

const char* kDBusService = "org.voiceassistant.App";
const char* kDBusPath    = "/org/voiceassistant/App";

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("0 Б");
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 Б").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 КБ").arg(bytes / 1024.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 МБ").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

// Печатает путь и его состояние. Возвращает true, если файл на месте.
bool reportPath(const char* label, const QString& path, int* problems)
{
    if (path.isEmpty()) {
        std::printf("  %-8s : <не задан>\n", label);
        return false;
    }
    const QFileInfo fi(path);
    if (fi.exists() && fi.isFile()) {
        std::printf("  %-8s : %s  OK (%s)\n", label, qPrintable(path),
                    qPrintable(humanSize(fi.size())));
        return true;
    }
    std::printf("  %-8s : %s  НЕТ ФАЙЛА\n", label, qPrintable(path));
    if (problems) {
        ++(*problems);
    }
    return false;
}

int countLines(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return -1;
    }
    QTextStream in(&f);
    in.setCodec("UTF-8");
    int n = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            ++n;
        }
    }
    return n;
}

// Клавиша, которую с большой вероятностью использует само приложение или DE.
// Без EVIOCGRAB она доходит и до активного окна — поэтому для хоткея микрофона
// лучше брать то, чем никто не пользуется. Перехватывать из-за этого ВСЮ
// клавиатуру (grab=true) нельзя: диктовка не должна мешать печатать.
bool keyIsLikelyInUse(const QString& spec)
{
    int code = -1;
    quint32 mods = EvdevHotkeyListener::ModNone;
    if (!EvdevHotkeyListener::parseKey(spec, &code, &mods, nullptr)) {
        return false;          // спецификация и так будет показана как ошибочная
    }
    if (mods != EvdevHotkeyListener::ModNone) {
        return false;          // комбинация — почти наверняка свободна
    }
    if (code >= KEY_F13 && code <= KEY_F24) {
        return false;
    }
    switch (code) {
    case KEY_SCROLLLOCK:
    case KEY_PAUSE:
    case KEY_MICMUTE:
    case KEY_RIGHTCTRL:
    case KEY_RIGHTALT:
        return false;
    default:
        return true;
    }
}

// Собирает инжектор так же, как ApplicationController: те же ключи конфига,
// тот же порядок вызовов. Если здесь текст доходит, а в приложении нет —
// дело не в xdotool, а в том, какое окно запоминается в начале записи.
std::unique_ptr<XdotoolInjector> makeInjectorFromConfig(ConfigManager& cfg)
{
    auto inj = std::make_unique<XdotoolInjector>();
    const QString method = cfg.injectorMethod();
    if (method == QLatin1String("xdotool")) {
        inj->setMethod(XdotoolInjector::Method::XdotoolType);
    } else if (method == QLatin1String("clipboard")) {
        inj->setMethod(XdotoolInjector::Method::Clipboard);
    } else {
        inj->setMethod(XdotoolInjector::Method::Auto);
    }
    inj->setTypingDelayMs(cfg.typingDelayMs());
    inj->setPreserveClipboard(cfg.preserveClipboard());
    inj->setClipboardRestoreMs(cfg.clipboardRestoreMs());
    inj->setPinMode(stringToPinMode(cfg.pinMode()));
    inj->setPinActivateDelayMs(cfg.pinActivateMs());
    inj->setPinRestoreFocus(cfg.pinRestoreFocus());
    inj->setOwnWindowClasses(cfg.ownWindowClasses());
    inj->initialize();
    return inj;
}

ConfigManager* makeConfigFromArgs(const QStringList& args)
{
    QString iniPath;
    const int ci = args.indexOf(QStringLiteral("--config"));
    if (ci >= 0 && ci + 1 < args.size()) {
        iniPath = args.at(ci + 1);
    }
    return iniPath.isEmpty() ? new ConfigManager() : new ConfigManager(iniPath);
}

void printWindowLine(const char* label, const QString& wid)
{
    if (wid.isEmpty()) {
        std::printf("%-14s: не определено (нет X-сервера или оконного менеджера)\n", label);
        return;
    }
    const QString name  = XdotoolInjector::windowName(wid);
    const QString cls   = XdotoolInjector::windowClass(wid);
    const qint64  pid   = XdotoolInjector::windowPid(wid);
    std::printf("%-14s: %s  заголовок=%s  WM_CLASS=%s  pid=%s\n",
                label, qPrintable(wid),
                name.isEmpty() ? qPrintable(QStringLiteral("(пусто)")) : qPrintable(name),
                cls.isEmpty()  ? qPrintable(QStringLiteral("(нет)"))    : qPrintable(cls),
                pid ? qPrintable(QString::number(pid)) : qPrintable(QStringLiteral("?")));
}


int runDiagnostics(const QStringList& args)
{
    int problems = 0;

    // конфиг: --config <путь> или стандартный
    QString iniPath;
    const int ci = args.indexOf(QStringLiteral("--config"));
    if (ci >= 0 && ci + 1 < args.size()) {
        iniPath = args.at(ci + 1);
    }

    std::unique_ptr<ConfigManager> cfg(iniPath.isEmpty()
        ? new ConfigManager()
        : new ConfigManager(iniPath));

    std::printf("voice-assistant --check\n=======================\n\n");

    // --- конфиг ---
    const QFileInfo iniInfo(cfg->settingsPath());
    std::printf("Конфиг      : %s  %s\n", qPrintable(cfg->settingsPath()),
                iniInfo.exists() ? "[есть]" : "[НЕТ — будут значения по умолчанию]");
    if (!iniInfo.exists()) {
        ++problems;
    }
    const QDir modelsDir(cfg->modelsPath());
    std::printf("Модели      : %s  %s\n", qPrintable(cfg->modelsPath()),
                modelsDir.exists() ? "[есть]" : "[НЕТ]");
    if (!modelsDir.exists()) {
        ++problems;
    }
    std::printf("Каталог конф: %s\n\n", qPrintable(cfg->configDir()));

    // --- VAD ---
    std::printf("VAD\n");
    reportPath("model", cfg->sileroVadPath(), &problems);
    std::printf("  threshold=%.2f  min_silence=%.2f  min_speech=%.2f  window=%d  threads=%d\n\n",
                cfg->vadThreshold(), cfg->vadMinSilenceDuration(), cfg->vadMinSpeechDuration(),
                cfg->vadWindowSize(), cfg->vadNumThreads());

    // --- профили ASR ---
    const QStringList profiles = cfg->asrProfileNames();
    const QString active = cfg->activeAsrProfileName();
    std::printf("Профили ASR : %s\n", profiles.isEmpty()
        ? qPrintable(QStringLiteral("<пусто — проверьте [asr] profiles>"))
        : qPrintable(profiles.join(QStringLiteral(", "))));
    if (profiles.isEmpty()) {
        ++problems;
    }
    std::printf("Активный    : %s\n\n", active.isEmpty() ? qPrintable(QStringLiteral("<не задан>"))
                                                         : qPrintable(active));

    for (const QString& name : profiles) {
        const AsrProfile p = cfg->asrProfile(name);
        const bool isActive = (name == active);
        std::printf("  [%s]%s engine=%s threads=%d decoding=%s\n",
                    qPrintable(name), isActive ? " *" : "",
                    qPrintable(AsrProfile::engineToString(p.engine)),
                    p.numThreads, qPrintable(p.decodingMethod));

        int localProblems = 0;
        switch (p.engine) {
        case AsrProfile::Engine::Transducer:
            reportPath("encoder", p.encoderPath, &localProblems);
            reportPath("decoder", p.decoderPath, &localProblems);
            reportPath("joiner",  p.joinerPath,  &localProblems);
            reportPath("tokens",  p.tokensPath,  &localProblems);
            break;
        case AsrProfile::Engine::NemoCtc:
            reportPath("model",  p.ctcModelPath, &localProblems);
            reportPath("tokens", p.tokensPath,   &localProblems);
            break;
        case AsrProfile::Engine::Whisper:
            reportPath("encoder", p.encoderPath, &localProblems);
            reportPath("decoder", p.decoderPath, &localProblems);
            reportPath("tokens",  p.tokensPath,  &localProblems);
            std::printf("  language=%s\n", qPrintable(p.language));
            break;
        case AsrProfile::Engine::Unknown:
            std::printf("  engine не распознан (нужно transducer | nemo-ctc | whisper)\n");
            ++localProblems;
            break;
        }

        if (!p.bpeVocabPath.isEmpty()) {
            const bool ok = reportPath("bpe_vocab", p.bpeVocabPath, &localProblems);
            if (!ok) {
                std::printf("            -> подсказки словами работать не будут;\n"
                            "               словарь создаётся: python3 tools/gen_bpe_vocab.py "
                            "<bpe.model> <bpe.vocab>\n");
            } else if (p.modelingUnit != QLatin1String("bpe")) {
                std::printf("            -> modeling_unit='%s', для BPE-словаря нужно 'bpe'\n",
                            qPrintable(p.modelingUnit));
            }
        }

        QString err;
        if (!p.isValid(&err)) {
            std::printf("  ИТОГ: профиль НЕ готов — %s\n\n", qPrintable(err.split(QLatin1Char('\n')).first()));
        } else {
            std::printf("  ИТОГ: готов\n\n");
        }
        if (isActive) {
            problems += localProblems;
        }
    }

    // --- подсказки ---
    std::printf("Подсказки (hotwords)\n");
    const int cmdHw = countLines(cfg->commandsHotwordsPath());
    const int usrHw = countLines(cfg->userHotwordsPath());
    std::printf("  commands_hotwords.txt : %s\n",
                cmdHw < 0 ? qPrintable(QStringLiteral("нет файла"))
                          : qPrintable(QStringLiteral("%1 строк").arg(cmdHw)));
    std::printf("  user_hotwords.txt     : %s\n",
                usrHw < 0 ? qPrintable(QStringLiteral("нет файла"))
                          : qPrintable(QStringLiteral("%1 строк").arg(usrHw)));
    std::printf("  hotwords_score=%.1f\n\n", cfg->hotwordsScore());

    // --- ввод текста ---
    std::printf("Ввод текста\n");
    const QString xdotool = QStandardPaths::findExecutable(QStringLiteral("xdotool"));
    const QString xclip   = QStandardPaths::findExecutable(QStringLiteral("xclip"));
    const QString xsel    = QStandardPaths::findExecutable(QStringLiteral("xsel"));
    const QString wlcopy  = QStandardPaths::findExecutable(QStringLiteral("wl-copy"));
    std::printf("  xdotool : %s\n", xdotool.isEmpty() ? qPrintable(QStringLiteral("НЕ НАЙДЕН"))
                                                       : qPrintable(xdotool));
    if (xdotool.isEmpty()) {
        ++problems;
    }
    std::printf("  буфер   : %s\n", !xclip.isEmpty()  ? qPrintable(xclip)
                                  : !xsel.isEmpty()   ? qPrintable(xsel)
                                  : !wlcopy.isEmpty() ? qPrintable(wlcopy)
                                                      : qPrintable(QStringLiteral("нет (будет xdotool type — "
                                                                                 "кириллица зависит от раскладки!)")));
    XdotoolInjector inj;
    inj.setMethod(cfg->injectorMethod() == QLatin1String("xdotool")   ? XdotoolInjector::Method::XdotoolType
                : cfg->injectorMethod() == QLatin1String("clipboard") ? XdotoolInjector::Method::Clipboard
                                                                      : XdotoolInjector::Method::Auto);
    inj.initialize();
    std::printf("  метод   : %s (preserve_clipboard=%s, restore=%d мс)\n",
                qPrintable(inj.backendName()),
                cfg->preserveClipboard() ? "true" : "false",
                cfg->clipboardRestoreMs());
    std::printf("  pin_window: %s\n", cfg->pinWindow() ? "true" : "false");
    if (cfg->pinWindow()) {
        std::printf("  pin_mode  : %s (pin_activate_ms=%d, pin_restore_focus=%s)\n",
                    qPrintable(cfg->pinMode()), cfg->pinActivateMs(),
                    cfg->pinRestoreFocus() ? "true" : "false");
        std::printf("              -> вставка идёт в окно, активное В НАЧАЛЕ записи;\n"
                    "                 можно переключиться и печатать в другом файле.\n");
        if (stringToPinMode(cfg->pinMode()) == PinMode::Activate) {
            std::printf("                 activate: окно активируется, текст печатается\n"
                        "                 НАСТОЯЩИМИ событиями (работает везде), затем\n"
                        "                 фокус возвращается. Забирает фокус на ~50-150 мс.\n");
        } else {
            std::printf("                 sendevent: xdotool --window, фокус не трогаем.\n"
                        "                 ВНИМАНИЕ: такие события отбрасывают браузеры,\n"
                        "                 LibreOffice, Java и часть терминалов, а xdotool\n"
                        "                 всё равно возвращает 0. Проверить своё приложение:\n"
                        "                   ./src/voice-assistant --type \"раз\" --pin-active --delay 3000\n");
        }
        printWindowLine("  сейчас", XdotoolInjector::activeWindowId());
        std::printf("  подробно  : ./src/voice-assistant --pin-info\n");
    }
    std::printf("\n");

    // --- прочее ---
    // --- правописание ---
    std::printf("Правописание\n");
    if (!cfg->spellcheckEnabled()) {
        std::printf("  выключено ([spellcheck] enabled=false)\n");
    } else {
        HunspellChecker::Options sopt;
        sopt.lang           = cfg->spellcheckLang();
        sopt.dictionaryDir  = cfg->spellcheckDictionaryDir();
        sopt.maxSuggestions = cfg->spellcheckMaxSuggestions();
        HunspellChecker sc(sopt);
        if (sc.initialize()) {
            std::printf("  движок    : hunspell\n");
            std::printf("  словарь   : %s\n", qPrintable(sc.dictionaryPath()));
            // контрольная проверка: заведомо ошибочное слово
            const QStringList err = sc.check(QStringLiteral("прверка"));
            const QStringList sug = sc.suggest(QStringLiteral("прверка"));
            std::printf("  контроль  : \"прверка\" -> %s%s\n",
                        err.isEmpty() ? "НЕ найдена ошибка (словарь странный)" : "ошибка",
                        sug.isEmpty() ? "" : qPrintable(QStringLiteral(", варианты: ") + sug.join(QStringLiteral(", "))));
            if (err.isEmpty()) {
                ++problems;
            }
        } else {
            std::printf("  НЕДОСТУПНО: %s\n", qPrintable(sc.lastError()));
            ++problems;
        }
    }
    std::printf("\n");

    // --- хоткей ---
    std::printf("Хоткей\n");
    if (cfg->hotkeyBackend() == QLatin1String("off")) {
        std::printf("  backend   : off (управление из трея и по D-Bus)\n");
    } else {
        EvdevHotkeyListener::Options hopt;
        hopt.key         = cfg->hotkeyKey();
        hopt.grab        = cfg->hotkeyGrab();
        hopt.grabDevices = cfg->hotkeyGrabDevices();
        EvdevHotkeyListener hk(hopt);
        QString hkError;
        // configure() не трогает /dev/input — валидируется только спецификация клавиши
        if (!hk.configure(&hkError)) {
            std::printf("  клавиша   : %s  НЕ РАЗОБРАНА: %s\n",
                        qPrintable(cfg->hotkeyKey()), qPrintable(hkError));
            ++problems;
        } else {
            std::printf("  клавиша   : %s  OK\n", qPrintable(cfg->hotkeyKey()));
        }
        std::printf("  режим     : %s, grab=%s\n",
                    qPrintable(cfg->hotkeyMode()),
                    cfg->hotkeyGrab()
                        ? "true (ЭКСКЛЮЗИВНО для перечисленных устройств)"
                        : "false (только наблюдение — клавиатура не блокируется)");
        const QStringList grabDevs = cfg->hotkeyGrabDevices();
        std::printf("  grab_devices: %s\n",
                    grabDevs.isEmpty() ? qPrintable(QStringLiteral("<не задан>"))
                                       : qPrintable(grabDevs.join(QStringLiteral(", "))));
        if (cfg->hotkeyGrab()) {
            std::printf("              -> перехватывается ТОЛЬКО перечисленное, либо\n"
                        "                 устройства, не похожие на полноценную клавиатуру:\n"
                        "                 основная клавиатура останется рабочей в любом случае.\n");
        }
        if (!cfg->hotkeyGrab() && keyIsLikelyInUse(cfg->hotkeyKey())) {
            std::printf("  совет     : '%s' скорее всего занята приложениями, а без grab\n"
                        "              клавиша доходит и до активного окна. Свободные варианты:\n"
                        "              F13-F24, Scroll Lock, Pause, Mic Mute, правый Ctrl\n"
                        "              или комбинация (ctrl+alt+f8). Блокировать ради этого\n"
                        "              клавиатуру НЕ нужно: печатать во время диктовки\n"
                        "              должно быть можно всегда.\n",
                        qPrintable(cfg->hotkeyKey()));
        }
        if (QFileInfo::exists(QStringLiteral("/dev/input"))) {
            const QDir di(QStringLiteral("/dev/input"));
            const QStringList events =
                di.entryList({QStringLiteral("event*")}, QDir::System | QDir::Files);
            int readable = 0;
            for (const QString& e : events) {
                if (QFileInfo(di.filePath(e)).isReadable()) {
                    ++readable;
                }
            }
            std::printf("  /dev/input: %d устройств event*, доступно для чтения: %d\n",
                        static_cast<int>(events.size()), readable);
            if (readable == 0) {
                std::printf("              -> нужен доступ: sudo usermod -aG input $USER,"
                            " затем перелогин\n");
                ++problems;
            }
        } else {
            std::printf("  /dev/input: НЕТ (evdev недоступен)\n");
            ++problems;
        }
    }
    std::printf("\n");

    // --- заметки (цель вывода "notes") ---
    std::printf("Заметки\n");
    if (!cfg->notesEnabled()) {
        std::printf("  выключено ([notes] enabled=false)\n");
    } else {
        const QString notesFile = cfg->notesFile();
        const QString notesDir  = notesFile.isEmpty() ? cfg->notesDir()
                                                      : QFileInfo(notesFile).absolutePath();
        std::printf("  каталог   : %s  %s\n", qPrintable(notesDir),
                    QDir(notesDir).exists()
                        ? (QFileInfo(notesDir).isWritable() ? "[есть, доступен для записи]"
                                                            : "[есть, НЕТ прав на запись]")
                        : "[будет создан]");
        if (QDir(notesDir).exists() && !QFileInfo(notesDir).isWritable()) {
            ++problems;
        }
        FileInjector::Options nopt;
        nopt.dir            = cfg->notesDir();
        nopt.file           = cfg->notesFile();
        nopt.timestampFormat = cfg->notesTimestampFormat();
        nopt.markdown       = cfg->notesMarkdown();
        nopt.dayHeader      = cfg->notesDayHeader();
        const FileInjector probe(nopt);
        const QString target = probe.filePath();
        std::printf("  файл      : %s  %s\n", qPrintable(target),
                    QFileInfo::exists(target) ? "[есть]" : "[появится после первой записи]");
        std::printf("  метка     : %s, markdown=%s, заголовок дня=%s\n",
                    nopt.timestampFormat.isEmpty()
                        ? qPrintable(QStringLiteral("нет"))
                        : qPrintable(nopt.timestampFormat),
                    nopt.markdown ? "true" : "false",
                    nopt.dayHeader ? "true" : "false");
        std::printf("  команды   : %s\n",
                    cfg->notesVoiceCommands()
                        ? qPrintable(CommandDictionary::targetCommandPhrases()
                                         .join(QStringLiteral(", ")))
                        : qPrintable(QStringLiteral("выключены ([notes] voice_commands=false) — "
                                                      "переключение из трея и по D-Bus")));
        std::printf("  цель старта: %s\n", qPrintable(cfg->notesStartTarget()));
        std::printf("  из консоли : ./src/voice-assistant --note \"текст заметки\"\n");
        std::printf("  по D-Bus    : dbus-send --session --type=method_call "
                    "--dest=org.voiceassistant.App /org/voiceassistant/App "
                    "org.voiceassistant.App.setOutputTarget string:notes\n");
    }
    std::printf("\n");

    // --- запись микрофона ---
    std::printf("Запись микрофона\n");
    const QString recDir = cfg->micCheckDir();
    std::printf("  каталог   : %s  %s\n", qPrintable(recDir),
                QDir(recDir).exists() ? "[есть]" : "[будет создан]");
    std::printf("  источник  : %s (agc = то, что слышит ASR; raw = сырой микрофон)\n",
                qPrintable(cfg->micCheckSource()));
    std::printf("  из консоли: ./src/voice-assistant --record 15 ~/mic.wav\n\n");

    std::printf("Постобработка : auto_punctuate=%s capitalize=%s add_final_dot=%s "
                "voice_punctuation=%s\n",
                cfg->autoPunctuate() ? "true" : "false",
                cfg->capitalizeSentences() ? "true" : "false",
                cfg->addFinalDot() ? "true" : "false",
                cfg->voicePunctuation() ? "true" : "false");
    std::printf("Команды       : editing_in_dictation=%s\n",
                cfg->editingCommandsInDictation() ? "true" : "false");
    std::printf("Аудио         : debug_log=%s typing_guard_ms=%d%s\n",
                cfg->audioDebugLog() ? "true" : "false",
                cfg->typingGuardMs(),
                cfg->typingGuardMs() > 0
                    ? (cfg->hotkeyBackend() == QLatin1String("evdev")
                           ? " (микрофон глушится, пока идёт печать)"
                           : " (НЕ СРАБОТАЕТ: нужен [hotkey] backend=evdev)")
                    : " (выключен)");
    std::printf("Сессия        : XDG_SESSION_TYPE=%s\n",
                qPrintable(qEnvironmentVariable("XDG_SESSION_TYPE",
                                                QStringLiteral("(не задан)"))));

    std::printf("\nИТОГ: %s\n", problems == 0
        ? qPrintable(QStringLiteral("всё на месте"))
        : qPrintable(QStringLiteral("проблем: %1 (см. строки «НЕТ ФАЙЛА» и docs/troubleshooting.md)")
                        .arg(problems)));
    return problems == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Диагностика привязки к окну: --pin-info и --type
//
// Зачем. Главный симптом поломки вывода — «в логе всё успешно, а в редакторе
// ничего». Чинится он только одним способом: сделать видимым, КУДА именно
// уходит текст. Обе опции работают без микрофона, без моделей и без GUI,
// поэтому проверяют ровно ту часть тракта, которая и ломается.
// ---------------------------------------------------------------------------

int runPinInfo(const QStringList& args)
{
    std::unique_ptr<ConfigManager> cfg(makeConfigFromArgs(args));

    std::printf("voice-assistant --pin-info\n==========================\n\n");
    std::printf("Конфиг        : %s\n", qPrintable(cfg->settingsPath()));
    std::printf("XDG_SESSION_TYPE=%s\n",
                qPrintable(qEnvironmentVariable("XDG_SESSION_TYPE",
                                                QStringLiteral("(не задан)"))));
    std::printf("DISPLAY       : %s\n",
                qPrintable(qEnvironmentVariable("DISPLAY", QStringLiteral("(не задан)"))));

    printWindowLine("Активное окно", XdotoolInjector::activeWindowId());

    std::printf("\n[output]\n");
    std::printf("method        : %s\n", qPrintable(cfg->injectorMethod()));
    std::printf("pin_window    : %s\n", cfg->pinWindow() ? "true" : "false");
    std::printf("pin_mode      : %s\n", qPrintable(cfg->pinMode()));
    std::printf("pin_activate_ms=%d\n", cfg->pinActivateMs());
    std::printf("pin_restore_focus=%s\n", cfg->pinRestoreFocus() ? "true" : "false");
    std::printf("own_window_class=%s\n", qPrintable(cfg->ownWindowClasses().join(QStringLiteral(", "))));

    std::unique_ptr<XdotoolInjector> inj(makeInjectorFromConfig(*cfg));
    if (!inj->isAvailable()) {
        std::printf("\nНЕТ xdotool — вывод текста невозможен (sudo apt install xdotool)\n");
        return 1;
    }
    std::printf("\nИнжектор      : %s\n", qPrintable(inj->backendName()));
    std::printf("Способ вставки: %s\n", qPrintable(inj->methodDescription()));

    // Какое окно запомнил бы контроллер в начале записи
    const QString wid = XdotoolInjector::activeWindowId();
    std::printf("\nЧто запомнил бы pin_window в начале записи:\n");
    if (wid.isEmpty()) {
        std::printf("  НИЧЕГО — активное окно не определяется, привязки не будет.\n"
                    "  Так бывает в Wayland-сессии (xdotool видит только XWayland)\n"
                    "  или если не запущен оконный менеджер.\n");
        return 1;
    }
    const QString cls = XdotoolInjector::windowClass(wid);
    const bool isSelf = (!cls.isEmpty() && cfg->ownWindowClasses().contains(cls))
                        || XdotoolInjector::windowPid(wid) == QCoreApplication::applicationPid();
    if (isSelf) {
        std::printf("  ОТКАЗ: окно %s принадлежит самому помощнику (WM_CLASS=%s).\n"
                    "  Привязка к нему НЕ устанавливается: текст ушёл бы в меню\n"
                    "  трея и пропал вместе с ним. Начинайте запись хоткеем.\n",
                    qPrintable(wid), qPrintable(cls));
        return 1;
    }
    std::printf("  окно %s — привязка установлена\n", qPrintable(wid));

    std::printf("\nПроверка вставки:\n"
                "  1) оставайтесь в редакторе и выполните\n"
                "       ./src/voice-assistant --type \"проверка раз\" --pin-active --delay 3000\n"
                "  2) за 3 секунды переключитесь в ДРУГОЕ окно (имитация «фокус ушёл»)\n"
                "  3) текст должен появиться в редакторе, а фокус — вернуться обратно\n"
                "  Если текста нет — смотрите предупреждения в выводе команды: там\n"
                "  написано, какое окно выбрано и каким способом идёт вставка.\n"
                "  См. docs/troubleshooting.md, раздел «pin_window не печатает».\n");
    return 0;
}

int runTypeTest(QCoreApplication& app, const QStringList& args)
{
    const int ti = args.indexOf(QStringLiteral("--type"));
    const QString text = (ti >= 0 && ti + 1 < args.size()) ? args.at(ti + 1) : QString();
    if (text.isEmpty()) {
        std::fprintf(stderr,
                     "--type: нужен текст.\n"
                     "Пример: ./src/voice-assistant --type \"проверка раз\" --pin-active --delay 3000\n");
        return 2;
    }

    int delayMs = 0;
    const int di = args.indexOf(QStringLiteral("--delay"));
    if (di >= 0 && di + 1 < args.size()) {
        bool ok = false;
        const int v = args.at(di + 1).toInt(&ok);
        if (ok && v >= 0 && v <= 60000) {
            delayMs = v;
        } else {
            std::fprintf(stderr, "--delay: нужно число миллисекунд 0..60000\n");
            return 2;
        }
    }
    const bool pinActive = args.contains(QStringLiteral("--pin-active"));
    QString pinWid;
    const int wi = args.indexOf(QStringLiteral("--window"));
    if (wi >= 0 && wi + 1 < args.size()) {
        pinWid = args.at(wi + 1);
    }

    std::unique_ptr<ConfigManager> cfg(makeConfigFromArgs(args));
    std::unique_ptr<XdotoolInjector> inj(makeInjectorFromConfig(*cfg));
    if (!inj->isAvailable()) {
        std::fprintf(stderr,
                     "--type: xdotool не найден — вставлять текст нечем "
                     "(sudo apt install xdotool)\n");
        return 1;
    }

    std::printf("Конфиг        : %s\n", qPrintable(cfg->settingsPath()));
    std::printf("Способ вставки: %s\n", qPrintable(inj->methodDescription()));
    std::printf("pin_mode      : %s (pin_window=%s)\n",
                qPrintable(cfg->pinMode()), cfg->pinWindow() ? "true" : "false");
    printWindowLine("Активное окно", XdotoolInjector::activeWindowId());

    if (pinActive) {
        pinWid = XdotoolInjector::activeWindowId();
    }
    if (!pinWid.isEmpty()) {
        inj->setPinnedWindow(pinWid);
        printWindowLine("Привязка", pinWid);
    } else {
        std::printf("Привязка      : нет (текст пойдёт в активное окно)\n");
    }

    if (delayMs > 0) {
        std::printf("\n%d мс на то, чтобы переключиться в другое окно...\n", delayMs);
        std::fflush(stdout);
    }

    int rc = 0;
    QTimer::singleShot(delayMs, [&]() {
        printWindowLine("Фокус сейчас", XdotoolInjector::activeWindowId());
        std::fflush(stdout);   // иначе строки инжектора (stderr) уедут выше
        const bool ok = inj->typeText(text);
        std::printf("\n%s\n", ok
            ? qPrintable(QStringLiteral("ВСТАВЛЕНО: «%1» (%2 симв.)").arg(text).arg(text.size()))
            : qPrintable(QStringLiteral("НЕ ВСТАВЛЕНО: %1").arg(inj->lastError())));
        std::fflush(stdout);
        // Даём целевому приложению забрать текст из буфера и дожить до
        // восстановления буфера/возврата фокуса.
        QTimer::singleShot(qMax(400, cfg->clipboardRestoreMs()), &app, &QCoreApplication::quit);
        if (!ok) {
            rc = 1;
        }
    });
    app.exec();
    return rc;
}

// --download-model <id|all|list> — загрузка моделей из консоли.
// Тот же путь, что и GUI-мастер: каталог в ModelCatalog, проверка sha256,
// докачка .part после обрыва. Нужно для установки без GUI (ssh, скрипты)
// и для тестировщиков, которые любят терминал.
int runDownloadModel(QCoreApplication& app, const QStringList& args)
{
    const int di = args.indexOf(QStringLiteral("--download-model"));
    const QString what = (di >= 0 && di + 1 < args.size()
                          && !args.at(di + 1).startsWith(QLatin1String("--")))
        ? args.at(di + 1) : QStringLiteral("list");

    std::unique_ptr<ConfigManager> cfg(makeConfigFromArgs(args));
    const QString destDir = cfg->modelsPath();

    if (what == QLatin1String("list")) {
        std::printf("Каталог моделей (установщик: %s)\n\n", qPrintable(destDir));
        for (const ModelPackage& p : modelCatalog()) {
            const bool installed = isModelPackageInstalled(p, destDir);
            std::printf("  %-14s %7.1f МБ  %s  %s\n", qPrintable(p.id),
                        p.sizeBytes / (1024.0 * 1024.0),
                        installed ? "[установлен]" : "[нет]        ",
                        qPrintable(p.title));
        }
        std::printf("\nСкачать:  voice-assistant --download-model zipformer-ru\n"
                    "Всё сразу: voice-assistant --download-model all\n"
                    "Заново:    voice-assistant --download-model <id> --force\n"
                    "(silero-vad обязателен и добавляется автоматически)\n");
        return 0;
    }

    QList<ModelPackage> queue;
    if (what == QLatin1String("all")) {
        queue = modelCatalog();
    } else {
        const ModelPackage* p = findModelPackage(what);
        if (!p) {
            std::fprintf(stderr,
                         "--download-model: неизвестная модель '%s'. Список: "
                         "voice-assistant --download-model list\n", qPrintable(what));
            return 2;
        }
        queue << *p;
        // VAD обязателен: без него не работает ни один профиль
        const ModelPackage* vad = findModelPackage(QStringLiteral("silero-vad"));
        if (vad && !isModelPackageInstalled(*vad, destDir)) {
            queue.prepend(*vad);
        }
    }

    ModelDownloader downloader;
    int exitCode = 0;
    QObject::connect(&downloader, &ModelDownloader::progress,
                     [](const QString& id, qint64 received, qint64 total) {
        const int pct = total > 0 ? int(received * 100 / total) : 0;
        std::printf("\r  %s: %6.1f / %.1f МБ (%3d%%)   ", qPrintable(id),
                    received / (1024.0 * 1024.0), total / (1024.0 * 1024.0), pct);
        std::fflush(stdout);
    });
    QObject::connect(&downloader, &ModelDownloader::packageFinished,
                     [](const QString& id) {
        std::printf("\r  ✔ %s установлен (sha256 совпал)                          \n",
                    qPrintable(id));
    });
    QObject::connect(&downloader, &ModelDownloader::packageFailed,
                     [&exitCode](const QString& id, const QString& error) {
        exitCode = 1;
        std::printf("\n  ✖ %s: %s\n", qPrintable(id), qPrintable(error));
    });
    QObject::connect(&downloader, &ModelDownloader::finished,
                     [&app, &exitCode, cfgPtr = cfg.get(), destDir](bool allOk) {
        if (allOk) {
            // Активный профиль не готов, а рекомендованный только что скачан —
            // делаем его активным, чтобы «скачал и сразу диктую» работало.
            const QString rec = recommendedInstalledProfile(destDir);
            const AsrProfile active = cfgPtr->asrProfile(cfgPtr->activeAsrProfileName());
            QString err;
            if (!rec.isEmpty() && !active.isValid(&err) && rec != cfgPtr->activeAsrProfileName()) {
                cfgPtr->setActiveAsrProfileName(rec);
                std::printf("Активный профиль ASR установлен: %s\n", qPrintable(rec));
            }
        } else if (exitCode == 0) {
            exitCode = 1;
        }
        app.quit();
    });

    std::printf("Куда: %s\n", qPrintable(destDir));
    // --force — переустановить, даже если каталог выглядит установленным
    // (лечит полураспакованные каталоги от старых версий/ручных tar).
    const bool force = args.contains(QStringLiteral("--force"));
    // ВАЖНО: запуск из singleShot(0), то есть уже внутри exec(). Если всё
    // уже установлено, finished() испускается синхронно — quit(), вызванный
    // ДО exec(), теряется, и консольная команда зависала бы навсегда
    // (поймано живым тестом: повторный --download-model после обрыва).
    QTimer::singleShot(0, [&downloader, queue, destDir, force]() {
        downloader.download(queue, destDir, force);
    });
    app.exec();
    return exitCode;
}

// ---------------------------------------------------------------------------
// --transcribe <файл> — разбор аудиозаписи из файла (WAV/MP3/RAW/OGG/FLAC…).
//
// Тот же боевой путь, что и у GUI-команды «разбери файл»: AudioFileDecoder
// (декодирование в моно 16 кГц) -> VoicePipeline (SileroVAD -> ASR ->
// постобработка). Отличия от tools/vad-asr-test: работает из установленного
// пакета, понимает MP3 и RAW, умеет вставлять результат в окно и в заметки.
//
// ВЫБОР МОДЕЛИ (зачем этот флаг вообще в CLI):
//   --config <ini>    другой settings.ini (по умолчанию ~/.config/voice-assistant/settings.ini)
//   --profile <имя>   профиль ASR из [asr] profiles того же конфига
//                     (zipformer-ru, gigaam-v3, gigaam-v3-ctc, whisper-base…);
//                     по умолчанию — активный профиль (тот же, что у диктовки)
//   --threads N       число потоков декодирования поверх профиля
// Полный список профилей показывает `voice-assistant --check`.
// Произвольные пути к моделям без конфига — по-прежнему у tools/vad-asr-test.
//
// Параметры RAW (PCM без контейнера — телефонные записи):
//   --rate N (8000)  --channels N (1)  --format s16le|s8u|f32le|alaw|ulaw
//
// Вывод: текст ВСЕГДА печатается в stdout (удобно для скриптов:
// `--transcribe call.mp3 > call.txt`), диагностика и прогресс — в stderr.
// Дополнительные цели:
//   --out <файл>      записать текст в файл (UTF-8)
//   --notes           дописать в файл заметок ([notes])
//   --insert          вставить в окно, активное в момент запуска (pin-механизм)
//   --hotwords <файл> подсказки (строка = слово/фраза)
//   --no-punct        без голосовой пунктуации, заглавных и финальной точки
int runTranscribe(QCoreApplication& app, const QStringList& args)
{
    auto argValue = [&args](const QString& name) -> QString {
        const int i = args.indexOf(name);
        return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
    };

    const int ti = args.indexOf(QStringLiteral("--transcribe"));
    const QString file = (ti >= 0 && ti + 1 < args.size()
                          && !args.at(ti + 1).startsWith(QLatin1String("--")))
        ? args.at(ti + 1) : QString();
    if (file.isEmpty()) {
        std::fprintf(stderr,
            "--transcribe: нужен аудиофайл.\n"
            "Пример:  voice-assistant --transcribe звонок.mp3 --profile gigaam-v3\n"
            "Флаги:   --config <ini> --profile <имя> --threads N --no-punct\n"
            "         --rate N --channels N --format s16le|s8u|f32le|alaw|ulaw  (RAW)\n"
            "         --out <файл.txt> --notes --insert --hotwords <файл>\n"
            "Текст — в stdout, диагностика — в stderr. Профили ASR: --check.\n");
        return 2;
    }
    if (!QFileInfo::exists(file)) {
        std::fprintf(stderr, "--transcribe: нет файла %s\n", qPrintable(file));
        return 1;
    }

    std::unique_ptr<ConfigManager> cfg(makeConfigFromArgs(args));
    VoicePipeline::Settings settings = VoicePipeline::loadSettings(*cfg);

    // --- выбор модели ---
    const QString profile = argValue(QStringLiteral("--profile"));
    if (!profile.isEmpty()) {
        if (!cfg->hasAsrProfile(profile)) {
            std::fprintf(stderr,
                "--transcribe: нет профиля '%s'. Доступны: %s\n",
                qPrintable(profile),
                qPrintable(cfg->asrProfileNames().join(QStringLiteral(", "))));
            return 2;
        }
        settings.asr = cfg->asrProfile(profile);
    }
    const QString threadsStr = argValue(QStringLiteral("--threads"));
    if (!threadsStr.isEmpty()) {
        bool ok = false;
        const int n = threadsStr.toInt(&ok);
        if (!ok || n < 1 || n > 64) {
            std::fprintf(stderr, "--transcribe: --threads нужно число 1..64\n");
            return 2;
        }
        settings.asr.numThreads = n;
    }
    if (args.contains(QStringLiteral("--no-punct"))) {
        settings.text.voicePunctuation    = false;
        settings.text.capitalizeSentences = false;
        settings.text.addFinalDot         = false;
    }

    // --- параметры RAW ---
    AudioFileDecoder::RawParams raw;
    raw.sampleRate = cfg->transcribeRawRate();
    raw.channels   = cfg->transcribeRawChannels();
    raw.format     = AudioFileDecoder::RawParams::formatFromString(cfg->transcribeRawFormat());
    const QString rateStr = argValue(QStringLiteral("--rate"));
    if (!rateStr.isEmpty()) {
        bool ok = false;
        const int v = rateStr.toInt(&ok);
        if (!ok || v < 1000 || v > 384000) {
            std::fprintf(stderr, "--transcribe: --rate нужно число 1000..384000\n");
            return 2;
        }
        raw.sampleRate = v;
    }
    const QString chStr = argValue(QStringLiteral("--channels"));
    if (!chStr.isEmpty()) {
        bool ok = false;
        const int v = chStr.toInt(&ok);
        if (!ok || v < 1 || v > 8) {
            std::fprintf(stderr, "--transcribe: --channels нужно число 1..8\n");
            return 2;
        }
        raw.channels = v;
    }
    const QString fmtStr = argValue(QStringLiteral("--format"));
    if (!fmtStr.isEmpty()) {
        if (!AudioFileDecoder::RawParams::formatNames().contains(fmtStr.toLower())) {
            std::fprintf(stderr, "--transcribe: --format может быть %s\n",
                         qPrintable(AudioFileDecoder::RawParams::formatNames()
                                        .join(QStringLiteral("|"))));
            return 2;
        }
        raw.format = AudioFileDecoder::RawParams::formatFromString(fmtStr);
    }

    // --- окно для --insert запоминаем ДО долгой работы ---
    const bool insert = args.contains(QStringLiteral("--insert"));
    std::unique_ptr<XdotoolInjector> injector;
    QString insertWid;
    if (insert) {
        injector = makeInjectorFromConfig(*cfg);
        if (!injector->isAvailable()) {
            std::fprintf(stderr,
                "--transcribe --insert: xdotool не найден — вставлять нечем\n");
            return 1;
        }
        insertWid = XdotoolInjector::activeWindowId();
        if (!insertWid.isEmpty()) {
            injector->setPinnedWindow(insertWid);
        }
        printWindowLine("Вставка в окно", insertWid);
    }

    // --- 1. декодирование файла (падаем рано, до загрузки модели) ---
    AudioFileDecoder::Audio audio;
    QString err;
    if (!AudioFileDecoder::decode(file, raw, audio, &err)) {
        std::fprintf(stderr, "--transcribe: %s\n", qPrintable(err));
        return 1;
    }
    if (audio.samples.empty()) {
        std::fprintf(stderr, "--transcribe: в файле нет аудио (0 сэмплов)\n");
        return 1;
    }
    std::fprintf(stderr, "Файл  : %s\n       %d Гц, %d кан., %s -> %.1f с @ 16 кГц моно\n",
                 qPrintable(file), audio.sourceRate, audio.sourceChannels,
                 qPrintable(audio.sourceFormat), audio.seconds());

    // --- 2. конвейер ---
    VoicePipeline pipeline(settings);
    QStringList segments;
    QObject::connect(&pipeline, &VoicePipeline::errorOccurred, [](const QString& m) {
        std::fprintf(stderr, "[ERROR] %s\n", qPrintable(m));
    });
    QObject::connect(&pipeline, &VoicePipeline::textReady,
                     [&segments](const QString& text) {
        const QString s = text.trimmed();
        if (!s.isEmpty()) {
            segments << s;
            std::fprintf(stderr, "  #%d: %s\n", segments.size(), qPrintable(s));
        }
    });

    if (!pipeline.initialize(&err)) {
        std::fprintf(stderr, "--transcribe: модель не запустилась: %s\n", qPrintable(err));
        return 1;
    }
    std::fprintf(stderr, "Модель: %s (%s), потоков: %d\n",
                 qPrintable(settings.asr.name),
                 qPrintable(AsrProfile::engineToString(settings.asr.engine)),
                 settings.asr.numThreads);

    const QString hotwordsFile = argValue(QStringLiteral("--hotwords"));
    if (!hotwordsFile.isEmpty()) {
        const QStringList hw = cfg->loadHotwords(hotwordsFile);
        pipeline.setHotwords(hw, cfg->hotwordsScore());
        std::fprintf(stderr, "Подсказки: %d шт. из %s\n",
                     static_cast<int>(hw.size()), qPrintable(hotwordsFile));
    }

    // --- 3. подача аудио окнами по 512 сэмплов (32 мс) ---
    QElapsedTimer total;
    total.start();
    const int windowBytes = 512 * static_cast<int>(sizeof(int16_t));
    const int totalBytes  = static_cast<int>(audio.samples.size() * sizeof(int16_t));
    const char* pcm = reinterpret_cast<const char*>(audio.samples.data());
    int lastPct = -1;
    for (int off = 0; off < totalBytes; off += windowBytes) {
        const int pct = static_cast<int>(100.0 * off / std::max(1, totalBytes));
        if (pct / 5 != lastPct / 5) {
            lastPct = pct;
            std::fprintf(stderr, "\rРаспознавание: %3d%%", pct);
            std::fflush(stderr);
        }
        const int n = std::min(windowBytes, totalBytes - off);
        pipeline.processAudio(QByteArray(pcm + off, n), 16000);
    }
    pipeline.flush();
    std::fprintf(stderr, "\rРаспознавание: 100%%\n");

    const QString text = segments.join(QLatin1Char('\n'));

    // --- 4. вывод ---
    // stdout — всегда (результат команды), остальное — дополнительные цели.
    if (!text.isEmpty()) {
        std::printf("%s\n", qPrintable(text));
        std::fflush(stdout);
    }

    const QString outFile = argValue(QStringLiteral("--out"));
    if (!outFile.isEmpty()) {
        QFile out(outFile);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "--out: не удалось записать %s: %s\n",
                         qPrintable(outFile), qPrintable(out.errorString()));
            return 1;
        }
        out.write(text.toUtf8());
        out.write("\n", 1);
        out.close();
        std::fprintf(stderr, "Записано: %s\n", qPrintable(outFile));
    }

    if (args.contains(QStringLiteral("--notes"))) {
        if (!cfg->notesEnabled()) {
            std::fprintf(stderr, "--notes: заметки выключены ([notes] enabled=false)\n");
            return 1;
        }
        FileInjector::Options nopt;
        nopt.dir            = cfg->notesDir();
        nopt.file           = cfg->notesFile();
        nopt.timestampFormat = cfg->notesTimestampFormat();
        nopt.markdown       = cfg->notesMarkdown();
        nopt.dayHeader      = cfg->notesDayHeader();
        FileInjector notes(nopt);
        if (!notes.initialize() || !notes.typeText(text)) {
            std::fprintf(stderr, "--notes: %s\n", qPrintable(notes.lastError()));
            return 1;
        }
        std::fprintf(stderr, "Заметки : %s\n", qPrintable(notes.filePath()));
    }

    int exitCode = segments.isEmpty() ? 1 : 0;
    if (insert && !text.isEmpty()) {
        const bool ok = injector->typeText(text);
        std::fprintf(stderr, "%s\n", ok
            ? qPrintable(QStringLiteral("Вставлено в окно %1 (%2 симв.)")
                             .arg(insertWid.isEmpty() ? QStringLiteral("(активное)") : insertWid)
                             .arg(text.size()))
            : qPrintable(QStringLiteral("НЕ вставлено: %1").arg(injector->lastError())));
        if (!ok) {
            exitCode = 1;
        }
        // Даём целевому приложению забрать текст из буфера, а инжектору —
        // восстановить буфер (внутренние таймеры живут от processEvents).
        const int waitMs = qMax(400, cfg->clipboardRestoreMs());
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < waitMs) {
            app.processEvents(QEventLoop::AllEvents, 100);
        }
    }

    std::fprintf(stderr, "Итог    : сегментов %d, аудио %.1f с, затрачено %.0f мс (RTF %.2f)\n",
                 segments.size(), audio.seconds(), static_cast<double>(total.elapsed()),
                 audio.seconds() > 0.0
                     ? (total.elapsed() / 1000.0) / audio.seconds() : 0.0);
    if (segments.isEmpty()) {
        std::fprintf(stderr,
            "Речь не найдена. Для тихих/шумных записей попробуйте --profile gigaam-v3,\n"
            "для RAW проверьте --rate/--format (неверная частота даёт кашу или тишину).\n");
    }
    return exitCode;
}

}  // namespace

int main(int argc, char *argv[])
{
    // --check не требует GUI: отдельный тип приложения, чтобы не поднимать X.
    // QCoreApplication::arguments(argc, argv) есть только в Qt6 — собираем сами.
    QStringList rawArgs;
    rawArgs.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        rawArgs << QString::fromLocal8Bit(argv[i]);
    }
    if (rawArgs.contains(QStringLiteral("--check"))) {
        QCoreApplication app(argc, argv);
        return runDiagnostics(QCoreApplication::arguments());
    }

    // --pin-info — диагностика вывода: какое окно сейчас активно, что запомнил
    // бы pin_window и каким способом пойдёт текст. Без микрофона и без GUI.
    if (rawArgs.contains(QStringLiteral("--pin-info"))) {
        QCoreApplication app(argc, argv);
        return runPinInfo(QCoreApplication::arguments());
    }

    // --download-model <id|all|list> [--config путь] — скачивание моделей
    // из консоли: тот же каталог, sha256 и докачка, что и в GUI-мастере.
    if (rawArgs.contains(QStringLiteral("--download-model"))) {
        QCoreApplication app(argc, argv);
        return runDownloadModel(app, QCoreApplication::arguments());
    }

    // --transcribe <файл> [--profile <имя>] [--config <ini>] — разбор записи
    // речи из аудиофайла (WAV/MP3/RAW/OGG/FLAC…). Тот же боевой конвейер, что
    // и у GUI-команды «разбери файл»; модель выбирается профилем из конфига.
    // QApplication нужен только для --insert (буфер обмена живёт в GUI-классе),
    // в остальном команда работает без X-сервера, как --check/--record.
    if (rawArgs.contains(QStringLiteral("--transcribe"))) {
        std::unique_ptr<QCoreApplication> app(
            rawArgs.contains(QStringLiteral("--insert"))
                ? static_cast<QCoreApplication*>(new QApplication(argc, argv))
                : new QCoreApplication(argc, argv));
        return runTranscribe(*app, QCoreApplication::arguments());
    }

    // --type "текст" [--pin-active] [--window WID] [--delay мс] [--config путь]
    // — проверить вставку отдельно от распознавания. QApplication нужен, чтобы
    // работал буфер обмена (сохранение и восстановление) — как в боевом режиме.
    if (rawArgs.contains(QStringLiteral("--type"))) {
        QApplication app(argc, argv);
        return runTypeTest(app, QCoreApplication::arguments());
    }

    // --record <секунды> [файл.wav] — сырая запись с микрофона, без GUI и без AGC.
    // Отделяет проблемы микрофона/AGC от проблем модели: тот же файл потом
    // прогоняется через ./tools/vad-asr-test всеми профилями.
    const int recordIdx = rawArgs.indexOf(QStringLiteral("--record"));
    if (recordIdx >= 0) {
        QCoreApplication app(argc, argv);
        const QStringList args = QCoreApplication::arguments();

        double seconds = 10.0;
        if (recordIdx + 1 < args.size()) {
            bool ok = false;
            const double v = args.at(recordIdx + 1).toDouble(&ok);
            if (ok && v > 0.0 && v <= 3600.0) {
                seconds = v;
            } else {
                std::fprintf(stderr,
                             "--record: недопустимая длительность '%s' (нужно 0.1..3600 с)\n",
                             qPrintable(args.at(recordIdx + 1)));
                return 2;
            }
        }
        const QString out = (recordIdx + 2 < args.size())
            ? args.at(recordIdx + 2)
            : QDir(QDir::homePath()).filePath(WavWriter::defaultFileName(QStringLiteral("mic-raw")));

        QtAudioCapture capture;
        WavWriter writer;
        bool opened = false;
        int  exitCode = 0;

        QObject::connect(&capture, &IAudioCapture::audioDataReady,
                         [&](const QByteArray& data, int sampleRate) {
            if (!opened) {
                if (!writer.open(out, sampleRate, 1, 16)) {
                    std::fprintf(stderr, "%s\n", qPrintable(writer.lastError()));
                    exitCode = 1;
                    app.quit();
                    return;
                }
                opened = true;
                std::printf("Пишу %s (%d Гц, моно, 16 бит), %.1f с...\n",
                            qPrintable(out), sampleRate, seconds);
                std::fflush(stdout);
            }
            writer.write(data);
        });
        QObject::connect(&capture, &IAudioCapture::errorOccurred, [&](const QString& msg) {
            std::fprintf(stderr, "Ошибка захвата: %s\n", qPrintable(msg));
            exitCode = 1;
            app.quit();
        });

        if (!capture.initialize()) {
            std::fprintf(stderr, "Не удалось инициализировать захват звука\n");
            return 1;
        }
        capture.start();

        QTimer::singleShot(static_cast<int>(seconds * 1000.0), [&]() {
            capture.stop();
            const double wrote = writer.seconds();
            const QString path = writer.filePath();
            const long long bytes = static_cast<long long>(writer.dataBytes());
            writer.close();
            std::printf("Сохранено: %s (%.2f с, %lld байт PCM)\n",
                        qPrintable(path), wrote, bytes);
            std::printf("Дальше:  ./tools/vad-asr-test %s --config "
                        "~/.config/voice-assistant/settings.ini\n", qPrintable(path));
            app.quit();
        });

        app.exec();
        return exitCode;
    }

    // --note <текст> [--config <путь>] — дописать строку в файл заметок и выйти.
    // Проверяет всю цепочку «конфиг -> FileInjector -> файл» без микрофона,
    // без GUI и без X-сервера: удобно убедиться, куда именно пойдут заметки,
    // до того как это понадобится в разговоре.
    const int noteIdx = rawArgs.indexOf(QStringLiteral("--note"));
    if (noteIdx >= 0) {
        QCoreApplication app(argc, argv);
        const QStringList args = QCoreApplication::arguments();

        const QString text = (noteIdx + 1 < args.size()) ? args.at(noteIdx + 1) : QString();
        if (text.trimmed().isEmpty()) {
            std::fprintf(stderr,
                         "--note: нужен текст заметки.\n"
                         "Пример: ./src/voice-assistant --note \"проверить AGC на тихом микрофоне\"\n");
            return 2;
        }

        QString iniPath;
        const int ci = args.indexOf(QStringLiteral("--config"));
        if (ci >= 0 && ci + 1 < args.size()) {
            iniPath = args.at(ci + 1);
        }
        std::unique_ptr<ConfigManager> cfg(iniPath.isEmpty() ? new ConfigManager()
                                                            : new ConfigManager(iniPath));
        if (!cfg->notesEnabled()) {
            std::fprintf(stderr, "--note: заметки выключены ([notes] enabled=false)\n");
            return 1;
        }

        FileInjector::Options nopt;
        nopt.dir            = cfg->notesDir();
        nopt.file           = cfg->notesFile();
        nopt.timestampFormat = cfg->notesTimestampFormat();
        nopt.markdown       = cfg->notesMarkdown();
        nopt.dayHeader      = cfg->notesDayHeader();

        FileInjector notes(nopt);
        if (!notes.initialize()) {
            std::fprintf(stderr, "--note: %s\n", qPrintable(notes.lastError()));
            return 1;
        }
        if (!notes.typeText(text)) {
            std::fprintf(stderr, "--note: %s\n", qPrintable(notes.lastError()));
            return 1;
        }
        std::printf("Записано: %s\n", qPrintable(notes.filePath()));
        return 0;
    }

    QApplication app(argc, argv);
    app.setApplicationName("voice-assistant");
#ifndef VOICE_ASSISTANT_VERSION
#define VOICE_ASSISTANT_VERSION "0.0.0-dev"   // вне CMake-сборки (например, ручной g++)
#endif
    app.setApplicationVersion(QStringLiteral(VOICE_ASSISTANT_VERSION));

    // Трей-приложение: значок в лотке окном НЕ считается, поэтому без этой
    // строки закрытие ЛЮБОГО диалога (Настройки, Редактор подсказок) Qt трактует
    // как «последнее окно закрыто» и молча завершает приложение — с кодом 0,
    // без единой ошибки в логе. Выход только через «Выход» в меню лотка.
    // Поймано в поле: смена модели в настройках «закрывала» помощника
    // (gdb: «exited normally» сразу за «Хоткей перезапущен»).
    app.setQuitOnLastWindowClosed(false);

    ApplicationController controller;
    TrayIcon trayIcon(&controller);
    trayIcon.show();

    if (!controller.isReady()) {
        qCritical().noquote()
            << QStringLiteral("Речевой тракт не запущен: %1\n"
                              "Запустите './src/voice-assistant --check', чтобы увидеть, "
                              "каких файлов не хватает.").arg(controller.lastError());
    }

    // Первый запуск после установки пакета: моделей нет — сразу показываем
    // мастер загрузки (отложенно, чтобы лоток успел появиться). Отказ не
    // фатален: пункт «Скачать модели...» в меню лотка остаётся, а консольный
    // путь — voice-assistant --download-model zipformer-ru.
    {
        const ConfigManager cfgForModels;
        if (!QFileInfo::exists(cfgForModels.sileroVadPath())) {
            qInfo().noquote()
                << QStringLiteral("Модели не найдены (%1) — открываю мастер загрузки; "
                                  "из консоли: voice-assistant --download-model list")
                       .arg(cfgForModels.modelsPath());
            QTimer::singleShot(700, &trayIcon, &TrayIcon::showModelDownloader);
        }
    }

    // D-Bus: адаптор соответствует src/dbus/org.voiceassistant.App.xml.
    // Отсутствие сессии (чистая TTY, systemd-юнит без bus) не должно ронять приложение.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        new DBusInterface(&controller);   // адаптор становится потомком контроллера

        if (!bus.registerObject(kDBusPath, &controller)) {
            qWarning().noquote() << QString("D-Bus: не зарегистрировал объект %1: %2")
                                        .arg(kDBusPath, bus.lastError().message());
        } else if (!bus.registerService(kDBusService)) {
            qWarning().noquote() << QString("D-Bus: не зарегистрировал сервис %1: %2 "
                                            "(возможно, помощник уже запущен)")
                                        .arg(kDBusService, bus.lastError().message());
        } else {
            qInfo().noquote() << QString("D-Bus: %1 на %2")
                                     .arg(kDBusService, kDBusPath);
        }
    } else {
        qWarning() << "D-Bus: сессионная шина недоступна, внешнее управление выключено";
    }

    return app.exec();
}
