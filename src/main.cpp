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

#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "output/XdotoolInjector.h"
#include "spellcheck/HunspellChecker.h"

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
    std::printf("  метод   : %s (preserve_clipboard=%s, restore=%d мс)\n\n",
                qPrintable(inj.backendName()),
                cfg->preserveClipboard() ? "true" : "false",
                cfg->clipboardRestoreMs());

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

    std::printf("Постобработка : auto_punctuate=%s voice_punctuation=%s\n",
                cfg->autoPunctuate() ? "true" : "false",
                cfg->voicePunctuation() ? "true" : "false");
    std::printf("Команды       : editing_in_dictation=%s\n",
                cfg->editingCommandsInDictation() ? "true" : "false");
    std::printf("Аудио         : debug_log=%s\n",
                cfg->audioDebugLog() ? "true" : "false");
    std::printf("Сессия        : XDG_SESSION_TYPE=%s\n",
                qPrintable(qEnvironmentVariable("XDG_SESSION_TYPE",
                                                QStringLiteral("(не задан)"))));

    std::printf("\nИТОГ: %s\n", problems == 0
        ? qPrintable(QStringLiteral("всё на месте"))
        : qPrintable(QStringLiteral("проблем: %1 (см. строки «НЕТ ФАЙЛА» и docs/troubleshooting.md)")
                        .arg(problems)));
    return problems == 0 ? 0 : 1;
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

    QApplication app(argc, argv);
    app.setApplicationName("voice-assistant");
    app.setApplicationVersion("0.0.1");

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
