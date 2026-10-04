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
#include "audio/WavWriter.h"
#include "commands/CommandDictionary.h"
#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "input/EvdevHotkeyListener.h"
#include "output/FileInjector.h"
#include "output/XdotoolInjector.h"

#include <QTimer>
#include "spellcheck/HunspellChecker.h"

#include <linux/input-event-codes.h>

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
    app.setApplicationVersion("0.0.1");

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
