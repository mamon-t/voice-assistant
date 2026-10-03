// tests/test_voice_units.cpp
//
// Проверки новых модулей: CommandParser/CommandDictionary, TextPostProcessor,
// XdotoolInjector (в режиме VOICE_ASSISTANT_DRYRUN=1 — без X-сервера).

#include <QtTest/QtTest>
#include <QTemporaryFile>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include "commands/CommandDictionary.h"
#include "commands/CommandParser.h"
#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "output/XdotoolInjector.h"
#include "text/TextPostProcessor.h"

class TestVoiceUnits : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // инжектор не должен ничего отправлять в систему
        qputenv("VOICE_ASSISTANT_DRYRUN", "1");
    }

    // ---------------- CommandParser ----------------

    void parseIsCaseAndPunctuationInsensitive()
    {
        CommandParser p;
        const auto c = p.parse(QStringLiteral("Удали слово."));
        QVERIFY(c.has_value());
        QCOMPARE(c->type, Command::Type::DeleteWord);
    }

    void parseModeCommands()
    {
        CommandParser p;
        const auto edit = p.parse(QStringLiteral("режим редактирования"));
        QVERIFY(edit.has_value());
        QCOMPARE(edit->type, Command::Type::SetMode);
        QCOMPARE(edit->mode, Mode::Edit);

        const auto dict = p.parse(QStringLiteral("режим диктовки"));
        QVERIFY(dict.has_value());
        QCOMPARE(dict->mode, Mode::Dictation);

        const auto off = p.parse(QStringLiteral("выключить"));
        QVERIFY(off.has_value());
        QCOMPARE(off->mode, Mode::Off);
    }

    void parsePunctuationCommand()
    {
        CommandParser p;
        const auto c = p.parse(QStringLiteral("вопросительный знак"));
        QVERIFY(c.has_value());
        QCOMPARE(c->type, Command::Type::Punctuation);
        QCOMPARE(c->argument, QStringLiteral("?"));
    }

    void parseRejectsNonCommands()
    {
        CommandParser p;
        QVERIFY(!p.parse(QStringLiteral("привет мир")).has_value());
        // «точка» внутри фразы — это не команда, её обработает TextPostProcessor
        QVERIFY(!p.parse(QStringLiteral("привет точка")).has_value());
        QVERIFY(!p.parse(QString()).has_value());
        QVERIFY(!p.parse(QStringLiteral("   ")).has_value());
    }

    void normalizeStripsPunctuation()
    {
        QCOMPARE(CommandParser::normalize(QStringLiteral("  УдаЛИ  слово! ")),
                 QStringLiteral("удали слово"));
        QCOMPARE(CommandParser::normalize(QStringLiteral("режим-правки")),
                 QStringLiteral("режим правки"));
    }

    void dictionaryLoadsCustomFile()
    {
        QTemporaryFile f;
        QVERIFY(f.open());
        {
            QTextStream out(&f);
            out.setCodec("UTF-8");
            // ВАЖНО: только QString. QTextStream::operator<<(const char*) трактует
            // байты как Latin-1, и кириллица превращается в двойную кодировку.
            out << QStringLiteral("# комментарий\n")
                << QStringLiteral("\n")
                << QStringLiteral("открой консоль = new-line\n")
                << QStringLiteral("шумный режим = set-mode:edit\n")
                << QStringLiteral("кавычка = punctuation:\"\n")
                << QStringLiteral("битая строка без равно\n")
                << QStringLiteral("неведомый тип = bogus-type\n");
        }
        f.close();

        CommandDictionary dict;
        dict.loadDefaults();
        const int before = dict.size();
        const int added = dict.loadFromFile(f.fileName());

        QCOMPARE(added, 3);                       // две битые строки отброшены
        QCOMPARE(dict.size(), before + 3);

        const auto nl = dict.find(QStringLiteral("открой консоль"));
        QVERIFY(nl.has_value());
        QCOMPARE(nl->type, Command::Type::NewLine);

        const auto mode = dict.find(QStringLiteral("шумный режим"));
        QVERIFY(mode.has_value());
        QCOMPARE(mode->mode, Mode::Edit);

        const auto punct = dict.find(QStringLiteral("кавычка"));
        QVERIFY(punct.has_value());
        QCOMPARE(punct->argument, QStringLiteral("\""));
    }

    void dictionaryPhrasesAreUsableAsHotwords()
    {
        CommandDictionary dict;
        dict.loadDefaults();
        const QStringList phrases = dict.phrases();
        QVERIFY(phrases.size() >= 20);
        QVERIFY(phrases.contains(QStringLiteral("удали слово")));
        QVERIFY(phrases.contains(QStringLiteral("режим редактирования")));
        // фразы уже нормализованы — их можно отдавать в hotwords как есть
        for (const QString& p : phrases) {
            QCOMPARE(p, CommandParser::normalize(p));
        }
    }

    // ---------------- TextPostProcessor ----------------

    void voicePunctuationInline()
    {
        TextPostProcessor pp;
        QCOMPARE(pp.process(QStringLiteral("привет точка это тест запятая всё работает")),
                 QStringLiteral("Привет. Это тест, всё работает."));
    }

    void multiwordPunctuationPhrase()
    {
        TextPostProcessor pp;
        // «вопросительный знак» должно съедаться целиком, а не как «вопросительный» + «знак»
        QCOMPARE(pp.process(QStringLiteral("всё работает вопросительный знак")),
                 QStringLiteral("Всё работает?"));
        // после ";" строчная буква правильна — предложение не закончилось
        QCOMPARE(pp.process(QStringLiteral("готово точка с запятой дальше")),
                 QStringLiteral("Готово; дальше."));
    }

    void newlineWordStartsNewSentence()
    {
        TextPostProcessor pp;
        QCOMPARE(pp.process(QStringLiteral("первая строка абзац вторая строка")),
                 QStringLiteral("Первая строка\nВторая строка."));
    }

    void spacingIsFixed()
    {
        TextPostProcessor pp;
        QCOMPARE(pp.process(QStringLiteral("привет   ,   мир")),
                 QStringLiteral("Привет, мир."));
        QCOMPARE(pp.process(QStringLiteral("открывающая скобка важное закрывающая скобка")),
                 QStringLiteral("(важное)."));
    }

    void noDoubleFinalDot()
    {
        TextPostProcessor pp;
        QCOMPARE(pp.process(QStringLiteral("готово точка")), QStringLiteral("Готово."));
        QCOMPARE(pp.process(QStringLiteral("готово!")),      QStringLiteral("Готово!"));
        QCOMPARE(pp.process(QStringLiteral("готово?")),      QStringLiteral("Готово?"));
    }

    void emptyAndWhitespaceInput()
    {
        TextPostProcessor pp;
        QCOMPARE(pp.process(QString()), QString());
        QCOMPARE(pp.process(QStringLiteral("   ")), QString());
        // из одних только слов-знаков не должно получаться «.»
        QCOMPARE(pp.process(QStringLiteral("точка")), QStringLiteral("."));
    }

    void optionsCanBeDisabled()
    {
        TextPostProcessor::Options opt;
        opt.voicePunctuation    = false;
        opt.capitalizeSentences = false;
        opt.addFinalDot         = false;
        const TextPostProcessor pp(opt);
        QCOMPARE(pp.process(QStringLiteral("привет точка мир")),
                 QStringLiteral("привет точка мир"));
    }

    void punctuationPhrasesForHotwords()
    {
        TextPostProcessor pp;
        const QStringList phrases = pp.punctuationPhrases();
        QVERIFY(phrases.contains(QStringLiteral("точка")));
        QVERIFY(phrases.contains(QStringLiteral("вопросительный знак")));
        QVERIFY(!phrases.contains(QStringLiteral("пробел")));   // его бустить не надо
    }

    // ---------------- ConfigManager ----------------

    void configReadsProfiles()
    {
        QTemporaryFile f;
        QVERIFY(f.open());
        {
            QTextStream out(&f);
            out.setCodec("UTF-8");
            out << QStringLiteral("[asr]\n")
                << QStringLiteral("active=ru\n")
                << QStringLiteral("profiles=ru,en\n")
                << QStringLiteral("num_threads=3\n")
                << QStringLiteral("\n[asr_ru]\n")
                << QStringLiteral("engine=transducer\n")
                << QStringLiteral("encoder=models/ru/encoder.int8.onnx\n")
                << QStringLiteral("decoder=/abs/ru/decoder.onnx\n")
                << QStringLiteral("joiner=~/ru/joiner.onnx\n")
                << QStringLiteral("tokens=ru/tokens.txt\n")
                << QStringLiteral("bpe_vocab=ru/bpe.vocab\n")
                << QStringLiteral("\n[asr_en]\n")
                << QStringLiteral("engine=whisper\n")
                << QStringLiteral("encoder=en/enc.onnx\n")
                << QStringLiteral("decoder=en/dec.onnx\n")
                << QStringLiteral("tokens=en/tokens.txt\n")
                << QStringLiteral("language=en\n")
                << QStringLiteral("\n[output]\n")
                << QStringLiteral("method=clipboard\n")
                << QStringLiteral("preserve_clipboard=false\n")
                << QStringLiteral("clipboard_restore_ms=250\n");
        }
        f.close();

        const ConfigManager cfg(f.fileName());

        // значение с запятыми QSettings отдаёт как QStringList — проверяем, что
        // ConfigManager это учитывает (на этом месте уже был баг)
        QCOMPARE(cfg.asrProfileNames(), (QStringList{QStringLiteral("ru"), QStringLiteral("en")}));
        QCOMPARE(cfg.activeAsrProfileName(), QStringLiteral("ru"));
        QVERIFY(cfg.hasAsrProfile(QStringLiteral("ru")));
        QVERIFY(!cfg.hasAsrProfile(QStringLiteral("нет-такого")));

        const AsrProfile ru = cfg.asrProfile(QStringLiteral("ru"));
        QCOMPARE(ru.engine, AsrProfile::Engine::Transducer);
        QCOMPARE(ru.numThreads, 3);                              // унаследован от [asr]
        QCOMPARE(ru.decodingMethod, QStringLiteral("modified_beam_search")); // дефолт transducer
        QCOMPARE(ru.modelingUnit, QStringLiteral("bpe"));        // подставился из-за bpe_vocab
        QVERIFY(ru.encoderPath.endsWith(QStringLiteral("/.voice_models/models/ru/encoder.int8.onnx")));
        QCOMPARE(ru.decoderPath, QStringLiteral("/abs/ru/decoder.onnx"));  // абсолютный не трогаем
        QVERIFY(ru.joinerPath.startsWith(QDir::homePath()));                // ~ раскрыт
        QVERIFY(!ru.isValid());                                  // файлов нет -> профиль не готов

        const AsrProfile en = cfg.asrProfile(QStringLiteral("en"));
        QCOMPARE(en.engine, AsrProfile::Engine::Whisper);
        QCOMPARE(en.language, QStringLiteral("en"));
        QCOMPARE(en.decodingMethod, QStringLiteral("greedy_search"));

        QCOMPARE(cfg.injectorMethod(), QStringLiteral("clipboard"));
        QVERIFY(!cfg.preserveClipboard());
        QCOMPARE(cfg.clipboardRestoreMs(), 250);
    }

    void configDefaultsWithoutFile()
    {
        const ConfigManager cfg(QStringLiteral("/tmp/нет-такого-файла-va.ini"));
        QVERIFY(cfg.autoPunctuate());          // дефолты должны быть рабочими
        QVERIFY(cfg.voicePunctuation());
        QVERIFY(cfg.preserveClipboard());
        QCOMPARE(cfg.clipboardRestoreMs(), 1000);
        QCOMPARE(cfg.vadThreshold(), 0.5f);
        QVERIFY(cfg.asrProfileNames().isEmpty());
    }

    // ---------------- XdotoolInjector (dry-run) ----------------

    void injectorClipboardOptions()
    {
        XdotoolInjector inj;
        inj.setPreserveClipboard(true);
        inj.setClipboardRestoreMs(250);
        QVERIFY(inj.initialize());
        QVERIFY(inj.typeText(QStringLiteral("тест")));   // в dry-run буфер не трогается
    }

    void injectorDryRun()
    {
        XdotoolInjector inj;
        QVERIFY(inj.initialize());
        QVERIFY(inj.isAvailable());
        QCOMPARE(inj.backendName(), QStringLiteral("dry-run"));

        QVERIFY(inj.typeText(QStringLiteral("привет, мир")));
        QVERIFY(inj.sendKey(QStringLiteral("ctrl+BackSpace")));
        QVERIFY(inj.sendKey(QStringLiteral("Return")));
        QVERIFY(inj.typeText(QString()));       // пустой текст — не ошибка
        QVERIFY(inj.sendKey(QString()));        // пустая клавиша — no-op, тоже не ошибка
    }
};

QTEST_MAIN(TestVoiceUnits)
#include "test_voice_units.moc"
