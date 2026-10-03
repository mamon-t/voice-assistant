// tests/test_voice_units.cpp
//
// Проверки новых модулей: CommandParser/CommandDictionary, TextPostProcessor,
// XdotoolInjector (в режиме VOICE_ASSISTANT_DRYRUN=1 — без X-сервера).

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QDir>

#include <linux/input.h>
#include <cstring>
#include <QFile>
#include <QTextStream>

#include "commands/CommandDictionary.h"
#include "commands/CommandParser.h"
#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "audio/WavWriter.h"
#include "input/EvdevHotkeyListener.h"
#include "output/XdotoolInjector.h"
#include "spellcheck/HunspellChecker.h"
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

    void leadingSpaceBetweenSegments()
    {
        // ровно тот случай из первого живого прогона: "…назад." + "Сегодня вот…"
        QVERIFY(TextPostProcessor::needsLeadingSpace(
            QStringLiteral("Это было двадцать лет назад."), QStringLiteral("Сегодня вот маленький.")));
        QVERIFY(TextPostProcessor::needsLeadingSpace(
            QStringLiteral("мастер спорта россии"), QStringLiteral("Россиянин.")));

        // пробел уже есть — второй не нужен
        QVERIFY(!TextPostProcessor::needsLeadingSpace(
            QStringLiteral("первая строка\n"), QStringLiteral("Вторая.")));
        QVERIFY(!TextPostProcessor::needsLeadingSpace(
            QStringLiteral("слово "), QStringLiteral("слово.")));

        // следующий сегмент начинается со знака — пробел перед ним не ставится
        QVERIFY(!TextPostProcessor::needsLeadingSpace(
            QStringLiteral("слово"), QStringLiteral(", продолжение")));

        // пустые строки
        QVERIFY(!TextPostProcessor::needsLeadingSpace(QString(), QStringLiteral("текст")));
        QVERIFY(!TextPostProcessor::needsLeadingSpace(QStringLiteral("текст"), QString()));
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

    // ---------------- HunspellChecker ----------------

    void spellSplitWords()
    {
        QCOMPARE(HunspellChecker::splitWords(QStringLiteral("Привет, мир! 2026 год.")),
                 (QStringList{QStringLiteral("Привет"), QStringLiteral("мир"),
                              QStringLiteral("год")}));
        // буквы с диакритикой не теряются, дефис разделяет
        QCOMPARE(HunspellChecker::splitWords(QStringLiteral("съешь ещё этих — кто-то")),
                 (QStringList{QStringLiteral("съешь"), QStringLiteral("ещё"),
                              QStringLiteral("этих"), QStringLiteral("кто"),
                              QStringLiteral("то")}));
        QVERIFY(HunspellChecker::splitWords(QString()).isEmpty());
        QVERIFY(HunspellChecker::splitWords(QStringLiteral("123 456")).isEmpty());
    }

    void spellCheckRealDictionary()
    {
        HunspellChecker c;
        if (!c.initialize()) {
            QSKIP("hunspell или словарь ru_RU не установлены — тест пропускается");
        }
        QVERIFY(c.isAvailable());
        QVERIFY(!c.dictionaryPath().isEmpty());

        // заведомо ошибочное слово находится, правильное — нет
        QCOMPARE(c.check(QStringLiteral("привет прверка мира")),
                 QStringList{QStringLiteral("прверка")});
        QVERIFY(c.check(QStringLiteral("проверка связи")).isEmpty());

        // повторы не дублируются
        QCOMPARE(c.check(QStringLiteral("прверка и ещё раз прверка")).size(), 1);

        // варианты исправления содержат правильный
        QVERIFY(c.suggest(QStringLiteral("прверка")).contains(QStringLiteral("проверка")));
        QVERIFY(c.suggest(QStringLiteral("прверка")).size() <= 5);   // max_suggestions по умолчанию

        // числа не считаются ошибками
        QVERIFY(c.check(QStringLiteral("2026 42 3.14")).isEmpty());
    }


    // ---------------- WavWriter ----------------

    void wavWriterHeaderFields()
    {
        const QByteArray h = WavWriter::makeHeader(48000, 2, 16, 192000);
        QCOMPARE(h.size(), 44);
        QCOMPARE(h.mid(0, 4),  QByteArray("RIFF"));
        QCOMPARE(h.mid(8, 4),  QByteArray("WAVE"));
        QCOMPARE(h.mid(12, 4), QByteArray("fmt "));
        QCOMPARE(h.mid(36, 4), QByteArray("data"));

        auto rd32 = [&h](int o) { quint32 v; std::memcpy(&v, h.constData() + o, 4); return qFromLittleEndian(v); };
        auto rd16 = [&h](int o) { quint16 v; std::memcpy(&v, h.constData() + o, 2); return qFromLittleEndian(v); };

        QCOMPARE(rd32(4),  quint32(192000 + 36));   // RIFF size
        QCOMPARE(rd16(20), quint16(1));             // PCM
        QCOMPARE(rd16(22), quint16(2));             // channels
        QCOMPARE(rd32(24), quint32(48000));         // sample rate
        QCOMPARE(rd32(28), quint32(48000 * 2 * 2)); // byte rate
        QCOMPARE(rd16(32), quint16(4));             // block align
        QCOMPARE(rd16(34), quint16(16));            // bits
        QCOMPARE(rd32(40), quint32(192000));        // data size
    }

    void wavWriterRoundTrip()
    {
        QTemporaryFile tf;
        QVERIFY(tf.open());
        const QString path = tf.fileName();
        tf.close();

        // 2 x 0.1 c осмысленных данных
        QByteArray pcm(3200, '\0');
        for (int i = 0; i < 1600; ++i) {
            const qint16 v = static_cast<qint16>((i * 37) % 20000 - 10000);
            std::memcpy(pcm.data() + i * 2, &v, 2);
        }

        {
            WavWriter w;
            QVERIFY(w.open(path, 16000, 1, 16));
            QVERIFY(w.isOpen());
            QVERIFY(w.write(pcm));
            QVERIFY(w.write(pcm));
            QCOMPARE(w.dataBytes(), static_cast<qint64>(6400));
            QCOMPARE(w.seconds(), 0.2);
            w.close();
        }

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray all = f.readAll();

        // размер = заголовок + данные
        QCOMPARE(all.size(), 44 + 6400);
        // заголовок проставлен при close() и совпадает с эталонным
        QCOMPARE(all.left(44), WavWriter::makeHeader(16000, 1, 16, 6400));
        // данные не повреждены
        QCOMPARE(all.mid(44), pcm + pcm);
        QCOMPARE(all.mid(44).size(), 6400);
    }

    void wavWriterRejectsBadInput()
    {
        WavWriter w;
        QVERIFY(!w.open(QString(), 16000));
        QVERIFY(!w.open(QStringLiteral("/tmp/va-test-bad.wav"), 16000, 1, 8));   // не 16 бит
        QVERIFY(!w.open(QStringLiteral("/tmp/va-test-bad.wav"), 0));
        QVERIFY(!w.write(QByteArray("данные")));   // не открыт
        QVERIFY(!w.isOpen());
        QVERIFY(!w.lastError().isEmpty());
    }

    void wavWriterFileNameHasTimestamp()
    {
        const QString a = WavWriter::defaultFileName(QStringLiteral("mic-check"));
        QVERIFY(a.startsWith(QStringLiteral("mic-check-")));
        QVERIFY(a.endsWith(QStringLiteral(".wav")));
        QCOMPARE(a.size(), int(QStringLiteral("mic-check-20261003-042501.wav").size()));
    }

    // ---------------- EvdevHotkeyListener ----------------

    void hotkeyParseKey()
    {
        int code = -1;
        quint32 mods = 12345;
        QString err;

        QVERIFY(EvdevHotkeyListener::parseKey(QStringLiteral("KEY_F8"), &code, &mods, &err));
        QCOMPARE(code, int(KEY_F8));
        QCOMPARE(mods, quint32(EvdevHotkeyListener::ModNone));

        QVERIFY(EvdevHotkeyListener::parseKey(QStringLiteral("f8"), &code, &mods, &err));
        QCOMPARE(code, int(KEY_F8));          // регистр и префикс KEY_ не важны

        QVERIFY(EvdevHotkeyListener::parseKey(QStringLiteral("66"), &code, &mods, &err));
        QCOMPARE(code, int(KEY_F8));          // числовой код: KEY_F8 == 66

        QVERIFY(EvdevHotkeyListener::parseKey(QStringLiteral("ctrl+space"), &code, &mods, &err));
        QCOMPARE(code, int(KEY_SPACE));
        QVERIFY(mods & EvdevHotkeyListener::ModCtrl);

        QVERIFY(EvdevHotkeyListener::parseKey(QStringLiteral("Ctrl+Alt+Shift+F13"), &code, &mods, &err));
        QCOMPARE(code, int(KEY_F13));
        QCOMPARE(mods, quint32(EvdevHotkeyListener::ModCtrl | EvdevHotkeyListener::ModAlt
                               | EvdevHotkeyListener::ModShift));

        QVERIFY(!EvdevHotkeyListener::parseKey(QStringLiteral("неведомая"), &code, &mods, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(!EvdevHotkeyListener::parseKey(QString(), &code, &mods, &err));
        QVERIFY(!EvdevHotkeyListener::parseKey(QStringLiteral("ctrl+неведомая"), &code, &mods, &err));
        QVERIFY(!EvdevHotkeyListener::parseKey(QStringLiteral("ctrl+ctrl"), &code, &mods, &err));
    }

    void hotkeyHandlesSyntheticEvents()
    {
        EvdevHotkeyListener::Options opt;
        opt.key = QStringLiteral("F8");
        EvdevHotkeyListener l(opt);
        QVERIFY(l.configure());          // без /dev/input — только разбор опций

        QSignalSpy pressed(&l, &IHotkeyListener::pressed);
        QSignalSpy released(&l, &IHotkeyListener::released);

        auto ev = [](quint16 type, quint16 code, qint32 value) {
            struct input_event e;
            std::memset(&e, 0, sizeof(e));
            e.type = type; e.code = code; e.value = value;
            return e;
        };

        QVERIFY(l.handleEvent(ev(EV_KEY, KEY_F8, 1)));
        QCOMPARE(pressed.count(), 1);

        // автоповтор (value=2) не должен порождать второе нажатие —
        // иначе push-to-talk дёргался бы непрерывно
        QVERIFY(!l.handleEvent(ev(EV_KEY, KEY_F8, 2)));
        QCOMPARE(pressed.count(), 1);

        QVERIFY(l.handleEvent(ev(EV_KEY, KEY_F8, 0)));
        QCOMPARE(released.count(), 1);

        // повторное отпускание без нажатия — ничего
        QVERIFY(!l.handleEvent(ev(EV_KEY, KEY_F8, 0)));
        QCOMPARE(released.count(), 1);

        // чужая клавиша и не-клавишные события игнорируются
        QVERIFY(!l.handleEvent(ev(EV_KEY, KEY_A, 1)));
        QVERIFY(!l.handleEvent(ev(EV_SYN, SYN_REPORT, 0)));
        QVERIFY(!l.handleEvent(ev(EV_REL, REL_X, 5)));
        QCOMPARE(pressed.count(), 1);
    }

    void hotkeyRequiresModifiers()
    {
        EvdevHotkeyListener::Options opt;
        opt.key = QStringLiteral("ctrl+space");
        EvdevHotkeyListener l(opt);
        QVERIFY(l.configure());

        QSignalSpy pressed(&l, &IHotkeyListener::pressed);
        auto ev = [](quint16 type, quint16 code, qint32 value) {
            struct input_event e;
            std::memset(&e, 0, sizeof(e));
            e.type = type; e.code = code; e.value = value;
            return e;
        };

        l.handleEvent(ev(EV_KEY, KEY_SPACE, 1));          // без Ctrl — не наша комбинация
        QCOMPARE(pressed.count(), 0);

        l.handleEvent(ev(EV_KEY, KEY_LEFTCTRL, 1));
        QVERIFY(l.handleEvent(ev(EV_KEY, KEY_SPACE, 1))); // с левым Ctrl
        QCOMPARE(pressed.count(), 1);

        l.handleEvent(ev(EV_KEY, KEY_SPACE, 0));
        l.handleEvent(ev(EV_KEY, KEY_LEFTCTRL, 0));
        l.handleEvent(ev(EV_KEY, KEY_RIGHTCTRL, 1));      // правый Ctrl тоже считается
        QVERIFY(l.handleEvent(ev(EV_KEY, KEY_SPACE, 1)));
        QCOMPARE(pressed.count(), 2);
    }

    void hotkeyConfigureRejectsBadKey()
    {
        EvdevHotkeyListener::Options opt;
        opt.key = QStringLiteral("такой-клавиши-нет");
        EvdevHotkeyListener l(opt);
        QString err;
        QVERIFY(!l.configure(&err));
        QVERIFY(err.contains(QStringLiteral("неизвестная клавиша")));
        QVERIFY(!l.isActive());
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
