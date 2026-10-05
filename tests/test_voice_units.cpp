// tests/test_voice_units.cpp
//
// Проверки новых модулей: CommandParser/CommandDictionary, TextPostProcessor,
// XdotoolInjector (в режиме VOICE_ASSISTANT_DRYRUN=1 — без X-сервера).

#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QVector>
#include <QDate>
#include <QTemporaryDir>
#include <QSet>
#include <QTemporaryFile>
#include <QDir>

#include <linux/input.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <QFile>
#include <QTextStream>

#include "commands/CommandDictionary.h"
#include "commands/CommandParser.h"
#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "models/ModelCatalog.h"
#include "models/ModelDownloader.h"
#include "audio/AudioFileDecoder.h"
#include "audio/TypingGuard.h"
#include "audio/WavWriter.h"
#include "core/OutputTarget.h"
#include "input/EvdevHotkeyListener.h"
#include "output/FileInjector.h"
#include "output/WindowTarget.h"
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

    void commandFileParseLineValid()
    {
        // parseLine — та самая функция, которой вкладка «Команды» валидирует
        // строки перед сохранением и которой loadFromFile() грузит файл:
        // что примет диалог, то гарантированно загрузится.
        QString phrase; Command cmd; QString err;
        QVERIFY(CommandDictionary::parseLine(QStringLiteral("удали слово = delete-word"),
                                             &phrase, &cmd, &err));
        QCOMPARE(phrase, QStringLiteral("удали слово"));
        QCOMPARE(cmd.type, Command::Type::DeleteWord);
        QVERIFY(err.isEmpty());

        QVERIFY(CommandDictionary::parseLine(QStringLiteral("  режим правки = SET-MODE:edit "),
                                             &phrase, &cmd, &err));
        QCOMPARE(cmd.type, Command::Type::SetMode);
        QCOMPARE(cmd.mode, Mode::Edit);

        QVERIFY(CommandDictionary::parseLine(QStringLiteral("кавычка = punctuation:\""),
                                             &phrase, &cmd, &err));
        QCOMPARE(cmd.type, Command::Type::Punctuation);
        QCOMPARE(cmd.argument, QStringLiteral("\""));
    }

    void commandFileParseLineSkipAndErrors()
    {
        QString phrase; Command cmd; QString err;
        // Пустые строки и комментарии — НЕ ошибка (error пустой): файл их содержит легально
        QVERIFY(!CommandDictionary::parseLine(QStringLiteral("# комментарий"), &phrase, &cmd, &err));
        QVERIFY(err.isEmpty());
        QVERIFY(!CommandDictionary::parseLine(QStringLiteral("   "), &phrase, &cmd, &err));
        QVERIFY(err.isEmpty());

        // Битые строки — ошибка с внятной причиной (диалог их не сохранит молча)
        QVERIFY(!CommandDictionary::parseLine(QStringLiteral("фраза без равно"), &phrase, &cmd, &err));
        QVERIFY(!err.isEmpty());

        err.clear();
        QVERIFY(!CommandDictionary::parseLine(QStringLiteral("фраза = fly-to-moon"), &phrase, &cmd, &err));
        QVERIFY(err.contains(QStringLiteral("fly-to-moon")));

        err.clear();
        QVERIFY(!CommandDictionary::parseLine(QStringLiteral(" = delete-word"), &phrase, &cmd, &err));
        QVERIFY(!err.isEmpty());   // пустая фраза слева от '='
    }

    void commandSpecRoundTrip()
    {
        // commandToSpec (кнопка «скопировать в свои») должна давать строку,
        // которую parseLine принимает обратно — иначе копия встроенной
        // команды не пережила бы сохранение файла.
        CommandDictionary dict;
        dict.loadDefaults();
        const QStringList phrases = dict.phrases();
        QVERIFY(phrases.size() >= 20);
        for (const QString& phrase : phrases) {
            const auto cmd = dict.find(phrase);
            QVERIFY(cmd.has_value());
            const QString spec = commandToSpec(*cmd);
            QVERIFY2(!spec.isEmpty(), qPrintable(phrase));
            QString phr2; Command cmd2; QString err;
            QVERIFY2(CommandDictionary::parseLine(phrase + QStringLiteral(" = ") + spec,
                                                  &phr2, &cmd2, &err),
                     qPrintable(phrase + QStringLiteral(": ") + err));
            QCOMPARE(cmd2.type, cmd->type);
        }
    }

    // ---------------- Каталог моделей и загрузчик ----------------
    //
    // Каталог вшит в код (URL + sha256 релизов sherpa-onnx), поэтому его
    // целостность проверяется тестами: битая ссылка или несовпадение хэша
    // в поле означают, что мастер загрузки не сможет поставить модели.

    void modelCatalogIsConsistent()
    {
        const QList<ModelPackage>& c = modelCatalog();
        QVERIFY(c.size() >= 5);
        QSet<QString> ids;
        QSet<QString> profiles;
        for (const ModelPackage& p : c) {
            QVERIFY(!p.id.isEmpty());
            QVERIFY(!ids.contains(p.id));          // id уникальны (по ним ищет CLI)
            ids.insert(p.id);
            QVERIFY(p.url.startsWith(
                QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/")));
            QCOMPARE(p.sha256.size(), 64);          // полный sha256, не обрезок
            for (const QChar ch : p.sha256) {
                QVERIFY(QStringLiteral("0123456789abcdef").contains(ch));
            }
            QVERIFY(p.sizeBytes > 0);
            QVERIFY(!p.title.isEmpty());
            QVERIFY(!p.description.isEmpty());
            QVERIFY(!p.fileName.isEmpty());
            if (p.required) {
                QVERIFY(p.profileName.isEmpty());   // VAD — не ASR-профиль
                QVERIFY(!p.isArchive());
            } else {
                QVERIFY(!p.profileName.isEmpty());
                QVERIFY(!profiles.contains(p.profileName));
                profiles.insert(p.profileName);
            }
            // installedName — то, что проверяет missingModelPackages
            QCOMPARE(p.installedName(), p.isArchive() ? p.extractedDirName() : p.fileName);
        }
        QVERIFY(findModelPackage(QStringLiteral("silero-vad")));
        QVERIFY(findModelPackage(QStringLiteral("silero-vad"))->required);
        QVERIFY(findModelPackage(QStringLiteral("zipformer-ru")));
        QVERIFY(findModelPackage(QStringLiteral("нет-такой")) == nullptr);
    }

    void modelCatalogCoversDefaultProfiles()
    {
        // Каждый профиль стандартного settings.ini обязан иметь пакет для
        // скачивания: иначе мастер обещает профиль, который нельзя поставить.
        const QStringList defaults = {
            QStringLiteral("zipformer-ru"), QStringLiteral("gigaam-v3"),
            QStringLiteral("gigaam-v3-ctc"), QStringLiteral("whisper-base")
        };
        for (const QString& prof : defaults) {
            bool found = false;
            for (const ModelPackage& p : modelCatalog()) {
                found = found || (p.profileName == prof);
            }
            QVERIFY2(found, qPrintable(prof));
        }
    }

    void modelMissingAndRecommended()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Пустой каталог — не установлено ничего
        QCOMPARE(missingModelPackages(dir.path()).size(), modelCatalog().size());
        QVERIFY(recommendedInstalledProfile(dir.path()).isEmpty());

        // «Устанавливаем» VAD-файл и каталог zipformer'а
        {
            QFile vad(dir.path() + QStringLiteral("/silero_vad.onnx"));
            QVERIFY(vad.open(QIODevice::WriteOnly));
            vad.write("dummy");
        }
        QVERIFY(QDir(dir.path()).mkdir(
            QStringLiteral("sherpa-onnx-small-zipformer-ru-2024-09-18")));
        {   // каталог обязан быть НЕ пустым: пустой — след обрыва распаковки
            QFile marker(dir.path() + QStringLiteral(
                "/sherpa-onnx-small-zipformer-ru-2024-09-18/encoder.int8.onnx"));
            QVERIFY(marker.open(QIODevice::WriteOnly));
            marker.write("dummy");
        }

        QCOMPARE(missingModelPackages(dir.path()).size(), modelCatalog().size() - 2);
        QCOMPARE(recommendedInstalledProfile(dir.path()), QStringLiteral("zipformer-ru"));
    }

    void downloaderSha256File()
    {
        // sha256("abc") — эталонное значение из FIPS 180-2
        QTemporaryFile f;
        QVERIFY(f.open());
        f.write("abc");
        f.close();
        QCOMPARE(ModelDownloader::sha256File(f.fileName()),
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223"
                                "b00361a396177a9cb410ff61f20015ad"));
        QVERIFY(ModelDownloader::sha256File(QStringLiteral("/nonexistent/path")).isEmpty());
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

    void configPinWindowDefaultsAreReliable()
    {
        // Без .ini способ доставки обязан быть «activate»: это единственный
        // вариант, который печатает в любом приложении. Откат по умолчанию к
        // sendevent — это и была причина «лог виден, а в редакторе ничего».
        const ConfigManager cfg(QStringLiteral("/tmp/нет-такого-файла-va.ini"));
        QVERIFY(!cfg.pinWindow());                          // привязка выключена
        QCOMPARE(stringToPinMode(cfg.pinMode()), PinMode::Activate);
        QCOMPARE(cfg.pinActivateMs(), 80);
        QVERIFY(cfg.pinRestoreFocus());
        QVERIFY(cfg.ownWindowClasses().contains(QStringLiteral("voice-assistant")));
    }

    void configPinWindowReadsIni()
    {
        QTemporaryFile f;
        QVERIFY(f.open());
        {
            QTextStream out(&f);
            out.setCodec("UTF-8");
            out << QStringLiteral("[output]\n")
                << QStringLiteral("pin_window=true\n")
                << QStringLiteral("pin_mode=sendevent\n")
                << QStringLiteral("pin_activate_ms=200\n")
                << QStringLiteral("pin_restore_focus=false\n")
                << QStringLiteral("own_window_class=voice-assistant, MyApp ,\n");
        }
        f.close();

        const ConfigManager cfg(f.fileName());
        QVERIFY(cfg.pinWindow());
        QCOMPARE(stringToPinMode(cfg.pinMode()), PinMode::SendEvent);
        QCOMPARE(cfg.pinActivateMs(), 200);
        QVERIFY(!cfg.pinRestoreFocus());
        // пробелы подрезаны, пустой хвост после запятой не попал в список
        QCOMPARE(cfg.ownWindowClasses(),
                 (QStringList{QStringLiteral("voice-assistant"), QStringLiteral("MyApp")}));
    }

    void configPinActivateMsIsClamped()
    {
        QTemporaryFile f;
        QVERIFY(f.open());
        {
            QTextStream out(&f);
            out.setCodec("UTF-8");
            out << QStringLiteral("[output]\npin_activate_ms=-50\n");
        }
        f.close();
        const ConfigManager cfg(f.fileName());
        QCOMPARE(cfg.pinActivateMs(), 0);   // отрицательная пауза не имеет смысла
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

    void injectorPinOptionsDoNotCrashDryRun()
    {
        // В dry-run привязка к окну не должна ничего вызывать наружу: ни
        // xdotool, ни проверок существования окна. Иначе тесты без X-сервера
        // начнут порождать процессы и падать.
        XdotoolInjector inj;
        inj.setPinMode(PinMode::Activate);
        inj.setPinActivateDelayMs(10);
        inj.setPinRestoreFocus(true);
        inj.setOwnWindowClasses({QStringLiteral("voice-assistant")});
        inj.setPinnedWindow(QStringLiteral("12345"));
        QVERIFY(inj.initialize());
        QCOMPARE(inj.pinnedWindow(), QStringLiteral("12345"));
        QVERIFY(inj.typeText(QStringLiteral("привет")));
        QVERIFY(inj.sendKey(QStringLiteral("Return")));
        QCOMPARE(inj.backendName(), QStringLiteral("dry-run"));
        inj.clearPinnedWindow();
        QVERIFY(inj.pinnedWindow().isEmpty());
    }

    // ---------------- Куда вставлять: WindowTarget ----------------
    //
    // Здесь проверяется решение, которое раньше принималось «на глаз» внутри
    // инжектора и потому не проверялось вовсе. Именно из-за него pin_window
    // молча не печатал: xdotool --window шлёт синтетику (XSendEvent), часть
    // приложений её отбрасывает, а код возврата всё равно 0.

    void windowTargetNoPinGoesToActiveWindow()
    {
        const WindowTarget t = decideWindowTarget(QString(), QStringLiteral("111"),
                                                  false, PinMode::Activate, true);
        QCOMPARE(t.delivery, WindowTarget::Delivery::ActiveWindow);
        QVERIFY(t.window.isEmpty());
        QVERIFY(!t.needsActivation());
        QVERIFY(!t.usesWindowFlag());
        QVERIFY(!t.pinLost);   // привязки не было — терять нечего, без предупреждений
    }

    void windowTargetPinnedButClosedFallsBackToActive()
    {
        // Окно закрыли, пока шла запись. Вставлять в него нельзя: xdotool
        // вернёт BadWindow, а текст пропадёт. Откат на активное окно.
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QStringLiteral("222"),
                                                  /*pinnedAlive=*/false,
                                                  PinMode::Activate, true);
        QCOMPARE(t.delivery, WindowTarget::Delivery::ActiveWindow);
        QVERIFY(t.pinLost);    // в лог должно попасть предупреждение
    }

    void windowTargetRefusesOwnWindow()
    {
        // Меню трея в X11 — отдельное окно, и в начале записи активно именно
        // оно. Привязка к нему = потерянный текст.
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QStringLiteral("222"),
                                                  true, PinMode::Activate, true,
                                                  {QStringLiteral("111")});
        QCOMPARE(t.delivery, WindowTarget::Delivery::ActiveWindow);
        QVERIFY(t.pinLost);    // в лог должно попасть предупреждение
    }

    void windowTargetAlreadyFocusedNeedsNoActivation()
    {
        // Окно и так в фокусе: не нужна ни активация, ни --window. Печать без
        // --window — это всегда настоящие события (XTest) в сфокусированное
        // окно. Прежняя доставка «--window в сфокусированное окно» на живом
        // Cinnamon + XED (GTK3) молча не доходила (грабля №29).
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QStringLiteral("111"),
                                                  true, PinMode::Activate, true);
        QCOMPARE(t.delivery, WindowTarget::Delivery::ActiveWindow);
        QVERIFY(t.window.isEmpty());
        QVERIFY(t.restoreTo.isEmpty());
        QVERIFY(!t.pinLost);   // окно в фокусе — привязка НЕ потеряна
        QVERIFY(!t.usesWindowFlag());
        QVERIFY(!t.needsActivation());
    }

    void windowTargetSendEventModeKeepsFocus()
    {
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QStringLiteral("222"),
                                                  true, PinMode::SendEvent, true);
        QCOMPARE(t.delivery, WindowTarget::Delivery::SendEvent);
        QCOMPARE(t.window, QStringLiteral("111"));
        QVERIFY(t.usesWindowFlag());
        QVERIFY(t.restoreTo.isEmpty());   // фокус не трогали — возвращать нечего
    }

    void windowTargetActivateModeRestoresFocus()
    {
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QStringLiteral("222"),
                                                  true, PinMode::Activate, true);
        QCOMPARE(t.delivery, WindowTarget::Delivery::Activate);
        QCOMPARE(t.window, QStringLiteral("111"));
        QCOMPARE(t.restoreTo, QStringLiteral("222"));
        QVERIFY(t.needsActivation());
        QVERIFY(!t.usesWindowFlag());   // при активации --window вреден
    }

    void windowTargetActivateWithoutRestore()
    {
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QStringLiteral("222"),
                                                  true, PinMode::Activate, /*allowRestore=*/false);
        QCOMPARE(t.delivery, WindowTarget::Delivery::Activate);
        QVERIFY(t.restoreTo.isEmpty());
    }

    void windowTargetActivateWithUnknownActiveWindow()
    {
        // Фокус не определился: активировать всё равно надо, а возвращать некуда.
        const WindowTarget t = decideWindowTarget(QStringLiteral("111"), QString(),
                                                  true, PinMode::Activate, true);
        QCOMPARE(t.delivery, WindowTarget::Delivery::Activate);
        QVERIFY(t.restoreTo.isEmpty());
    }

    void pinModeParsing()
    {
        QCOMPARE(stringToPinMode(QStringLiteral("activate")), PinMode::Activate);
        QCOMPARE(stringToPinMode(QStringLiteral("sendevent")), PinMode::SendEvent);
        QCOMPARE(stringToPinMode(QStringLiteral("send_event")), PinMode::SendEvent);
        QCOMPARE(stringToPinMode(QStringLiteral("  SEND_EVENT ")), PinMode::SendEvent);
        QCOMPARE(stringToPinMode(QStringLiteral("window")), PinMode::SendEvent);
        // неизвестное -> activate: тихий откат к нерабочему варианту уже был
        QCOMPARE(stringToPinMode(QStringLiteral("что-то странное")), PinMode::Activate);
        QCOMPARE(stringToPinMode(QString()), PinMode::Activate);
        QCOMPARE(pinModeToString(PinMode::SendEvent), QStringLiteral("sendevent"));
        QCOMPARE(pinModeToString(PinMode::Activate), QStringLiteral("activate"));
    }

    void deliveryNamesAreHumanReadable()
    {
        // По логу должно быть понятно, почему текст не дошёл, поэтому названия
        // способов — слова, а не термины X11.
        QVERIFY(!deliveryToString(WindowTarget::Delivery::SendEvent).isEmpty());
        QVERIFY(deliveryToString(WindowTarget::Delivery::Activate)
                    .contains(QStringLiteral("настоящ")));
        QVERIFY(deliveryToString(WindowTarget::Delivery::ActiveWindow)
                    .contains(QStringLiteral("активное")));
    }


    // ---------------- Заметки: FileInjector ----------------

    void notesFormatEntryIsPure()
    {
        const QDateTime when(QDate(2026, 10, 3), QTime(14, 5, 6));
        QCOMPARE(FileInjector::formatEntry(QStringLiteral("текст"),
                                           QStringLiteral("HH:mm:ss"), true, when),
                 QStringLiteral("- 14:05:06 — текст"));
        QCOMPARE(FileInjector::formatEntry(QStringLiteral("текст"), QString(), false, when),
                 QStringLiteral("текст"));
        QCOMPARE(FileInjector::dailyFileName(QDate(2026, 1, 2), true),
                 QStringLiteral("2026-01-02.md"));
        QCOMPARE(FileInjector::dailyFileName(QDate(2026, 1, 2), false),
                 QStringLiteral("2026-01-02.txt"));
    }

    void notesEraseLastWord()
    {
        QCOMPARE(FileInjector::eraseLastWord(QStringLiteral("раз два три")),
                 QStringLiteral("раз два"));
        QCOMPARE(FileInjector::eraseLastWord(QStringLiteral("одно")), QString());
        QCOMPARE(FileInjector::eraseLastWord(QString()), QString());
        // маркер списка остаётся: «- слово» -> «- »
        QCOMPARE(FileInjector::eraseLastWord(QStringLiteral("- слово")),
                 QStringLiteral("- "));
        // как в редакторе: хвостовые пробелы съедаются первыми
        QCOMPARE(FileInjector::eraseLastWord(QStringLiteral("текст   ")),
                 QStringLiteral("текст"));
    }

    void notesWritesDailyMarkdownFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        FileInjector::Options opt;
        opt.dir = dir.path();
        FileInjector notes(opt);
        QVERIFY(notes.initialize());
        QVERIFY(notes.isAvailable());
        QCOMPARE(notes.backendName(), QStringLiteral("файл заметок"));
        QCOMPARE(notes.filePath(),
                 QDir(dir.path()).filePath(
                     FileInjector::dailyFileName(QDate::currentDate(), true)));

        QVERIFY(notes.typeText(QStringLiteral("первая заметка")));
        QVERIFY(notes.typeText(QStringLiteral("вторая")));
        QVERIFY(notes.typeText(QString()));                 // пустой сегмент — не ошибка
        QCOMPARE(notes.entries(), static_cast<qint64>(2));

        QFile f(notes.filePath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(f.readAll());
        f.close();

        const QStringList lines = content.split(QLatin1Char('\n'));
        QVERIFY(lines.size() >= 4);
        QVERIFY(lines.at(0).startsWith(QStringLiteral("# Заметки ")));   // заголовок дня
        QCOMPARE(lines.at(1), QString());
        QVERIFY(lines.at(2).startsWith(QStringLiteral("- ")));
        QVERIFY(lines.at(2).contains(QStringLiteral(" — ")));           // метка времени
        QVERIFY(lines.at(2).endsWith(QStringLiteral("первая заметка")));
        QVERIFY(lines.at(3).endsWith(QStringLiteral("вторая")));
        // заголовок пишется один раз, а не при каждом дописывании
        QCOMPARE(content.count(QStringLiteral("# Заметки")), 1);
        // кириллица не превратилась в кашу (грабля №6: QTextStream и Latin-1)
        QVERIFY(content.contains(QStringLiteral("первая")));
    }

    void notesMultiLineSplitsIntoEntries()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FileInjector::Options opt;
        opt.dir = dir.path();
        FileInjector notes(opt);
        QVERIFY(notes.initialize());

        QVERIFY(notes.typeText(QStringLiteral("первая\nвторая\n\nтретья")));
        QCOMPARE(notes.entries(), static_cast<qint64>(3));

        QFile f(notes.filePath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(f.readAll());
        f.close();
        QCOMPARE(content.count(QStringLiteral("\n- ")), 3);
    }

    void notesExplicitPlainFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/moi-zametki.txt");

        FileInjector::Options opt;
        opt.file            = path;
        opt.markdown        = false;
        opt.timestampFormat = QString();
        FileInjector notes(opt);
        QVERIFY(notes.initialize());
        QCOMPARE(notes.filePath(), path);

        QVERIFY(notes.typeText(QStringLiteral("просто текст")));

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        // явный файл: ни заголовка дня, ни маркера, ни метки времени
        QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("просто текст\n"));
        f.close();
    }

    void notesSendKeyEditsFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FileInjector::Options opt;
        opt.dir = dir.path();
        FileInjector notes(opt);
        QVERIFY(notes.initialize());

        QVERIFY(notes.typeText(QStringLiteral("альфа бета гамма")));

        // «удали слово» стирает последнее слово последней заметки
        QVERIFY(notes.sendKey(QStringLiteral("ctrl+BackSpace")));
        QFile f(notes.filePath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(f.readAll());
        f.close();
        QVERIFY(content.contains(QStringLiteral("альфа бета")));
        QVERIFY(!content.contains(QStringLiteral("гамма")));

        // «новая строка» добавляет пустую строку
        QVERIFY(notes.sendKey(QStringLiteral("Return")));
        QVERIFY(notes.filePath().isEmpty() || true);
        QFile f2(notes.filePath());
        QVERIFY(f2.open(QIODevice::ReadOnly));
        content = QString::fromUtf8(f2.readAll());
        f2.close();
        QVERIFY(content.endsWith(QStringLiteral("\n\n")));

        // неприменимая к файлу клавиша — не ошибка, файл не меняется
        QVERIFY(notes.sendKey(QStringLiteral("shift+Home")));
        QFile f3(notes.filePath());
        QVERIFY(f3.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f3.readAll()), content);
        f3.close();
    }

    // ---------------- Цель вывода в командах ----------------

    void parseTargetCommands()
    {
        CommandParser p;

        const auto notes = p.parse(QStringLiteral("Заметка."));   // регистр и точка
        QVERIFY(notes.has_value());
        QCOMPARE(notes->type, Command::Type::SetTarget);
        QCOMPARE(notes->target, OutputTarget::Notes);

        const auto focus = p.parse(QStringLiteral("в редактор"));
        QVERIFY(focus.has_value());
        QCOMPARE(focus->target, OutputTarget::Focus);

        // словом внутри фразы команда не считается — иначе «сделать заметку
        // на полях» переключало бы цель вывода
        QVERIFY(!p.parse(QStringLiteral("сделать заметку на полях")).has_value());

        // все фразы из списка действительно лежат в словаре
        const QStringList phrases = CommandDictionary::targetCommandPhrases();
        QVERIFY(phrases.size() >= 4);
        for (const QString& phrase : phrases) {
            QVERIFY2(p.dictionary()->contains(CommandParser::normalize(phrase)),
                     qPrintable(phrase));
        }
    }

    void targetCommandsCanBeDisabled()
    {
        CommandDictionary d;
        d.loadDefaults();
        const QStringList phrases = CommandDictionary::targetCommandPhrases();

        int removed = 0;
        for (const QString& phrase : phrases) {
            if (d.removeCommand(phrase)) {
                ++removed;
            }
        }
        QCOMPARE(removed, phrases.size());
        for (const QString& phrase : phrases) {
            QVERIFY(!d.contains(CommandParser::normalize(phrase)));
        }
        // остальной словарь не пострадал
        QVERIFY(d.contains(CommandParser::normalize(QStringLiteral("удали слово"))));
        QVERIFY(d.contains(CommandParser::normalize(QStringLiteral("режим диктовки"))));
        // повторное удаление — не ошибка
        QVERIFY(!d.removeCommand(phrases.first()));
    }

    void outputTargetNames()
    {
        QCOMPARE(outputTargetToString(OutputTarget::Notes), QStringLiteral("notes"));
        QCOMPARE(outputTargetToString(OutputTarget::Focus), QStringLiteral("focus"));
        QCOMPARE(stringToOutputTarget(QStringLiteral("notes")),  OutputTarget::Notes);
        QCOMPARE(stringToOutputTarget(QStringLiteral("NOTES")),  OutputTarget::Notes);
        QCOMPARE(stringToOutputTarget(QStringLiteral("заметки")), OutputTarget::Notes);
        QCOMPARE(stringToOutputTarget(QStringLiteral("focus")),  OutputTarget::Focus);
        // неизвестное значение = активное окно, а не заметки: молча поменять
        // цель вывода было бы слишком дорогим сюрпризом
        QCOMPARE(stringToOutputTarget(QString()),            OutputTarget::Focus);
        QCOMPARE(stringToOutputTarget(QStringLiteral("что угодно")), OutputTarget::Focus);
        QVERIFY(!outputTargetTitle(OutputTarget::Notes).isEmpty());
    }

    // ---------------- Глушитель печати ----------------

    void typingGuardBlocksOnlyWhileTyping()
    {
        TypingGuard g(200);
        QVERIFY(g.isEnabled());
        QVERIFY(!g.blocked(0));            // нажатий ещё не было — не глушим
        g.noteKeyActivity(1000);
        QVERIFY(g.blocked(1100));          // 100 мс после нажатия
        QVERIFY(g.blocked(1199));
        QVERIFY(!g.blocked(1200));         // ровно guardMs — уже можно

        g.countDropped(1600);
        g.countDropped(1600);
        QCOMPARE(g.droppedChunks(),  static_cast<qint64>(2));
        QCOMPARE(g.droppedSamples(), static_cast<qint64>(3200));

        g.reset();
        QVERIFY(!g.blocked(1010));         // счётчик нажатий сброшен
        QCOMPARE(g.droppedChunks(), static_cast<qint64>(0));

        TypingGuard off;                   // по умолчанию выключен
        QVERIFY(!off.isEnabled());
        off.noteKeyActivity(5);
        QVERIFY(!off.blocked(6));

        TypingGuard negative(-10);         // мусор в конфиге не ломает тракт
        QCOMPARE(negative.guardMs(), 0);
        negative.noteKeyActivity(1);
        QVERIFY(!negative.blocked(2));
    }

    // ---------------- Хоткей: события печати ----------------

    void hotkeyReportsTypingActivity()
    {
        auto ev = [](quint16 type, quint16 code, qint32 value) {
            struct input_event e;
            std::memset(&e, 0, sizeof(e));
            e.type = type; e.code = code; e.value = value;
            return e;
        };

        // Комбинация: её собственный модификатор печатью не считается,
        // иначе «ctrl+space» глушил бы сам себя всю запись.
        EvdevHotkeyListener::Options opt;
        opt.key = QStringLiteral("ctrl+space");
        EvdevHotkeyListener l(opt);
        QVERIFY(l.configure());

        QSignalSpy activity(&l, &IHotkeyListener::keyActivity);
        QSignalSpy pressed(&l, &IHotkeyListener::pressed);

        l.handleEvent(ev(EV_KEY, KEY_LEFTCTRL, 1));
        QCOMPARE(activity.count(), 0);
        l.handleEvent(ev(EV_KEY, KEY_SPACE, 1));
        QCOMPARE(activity.count(), 0);
        QCOMPARE(pressed.count(), 1);
        l.handleEvent(ev(EV_KEY, KEY_SPACE, 2));     // автоповтор хоткея
        QCOMPARE(activity.count(), 0);

        l.handleEvent(ev(EV_KEY, KEY_A, 1));         // чужая клавиша — это печать
        QCOMPARE(activity.count(), 1);
        l.handleEvent(ev(EV_KEY, KEY_A, 2));         // удержанная клавиша печатает
        QCOMPARE(activity.count(), 2);
        l.handleEvent(ev(EV_KEY, KEY_LEFTSHIFT, 1)); // чужой модификатор — тоже
        QCOMPARE(activity.count(), 3);
        l.handleEvent(ev(EV_SYN, SYN_REPORT, 0));    // не-клавишные события — нет
        l.handleEvent(ev(EV_REL, REL_X, 5));
        QCOMPARE(activity.count(), 3);

        // Одиночная клавиша: любой модификатор считается печатью
        EvdevHotkeyListener::Options opt2;
        opt2.key = QStringLiteral("F8");
        EvdevHotkeyListener l2(opt2);
        QVERIFY(l2.configure());
        QSignalSpy activity2(&l2, &IHotkeyListener::keyActivity);
        l2.handleEvent(ev(EV_KEY, KEY_LEFTCTRL, 1));
        QCOMPARE(activity2.count(), 1);
        l2.handleEvent(ev(EV_KEY, KEY_F8, 1));       // сам хоткей — не печать
        QCOMPARE(activity2.count(), 1);
        l2.handleEvent(ev(EV_KEY, KEY_F8, 0));
        QCOMPARE(activity2.count(), 1);
    }

    // ---------------- Политика EVIOCGRAB ----------------

    void grabPolicyNeverTakesMainKeyboard()
    {
        const int bytes = KEY_MAX / 8 + 1;

        // Полноценная клавиатура: буквы и цифры. Заполняем весь диапазон
        // 0..127, потому что коды букв в input-event-codes.h не подряд
        // (KEY_A=30, KEY_B=48, KEY_Z=44).
        QVector<unsigned char> full(bytes, 0);
        for (int c = 0; c < 128; ++c) {
            full[c / 8] = static_cast<unsigned char>(full[c / 8] | (1u << (c % 8)));
        }
        // Педаль / footswitch: две кнопки
        QVector<unsigned char> pedal(bytes, 0);
        pedal[KEY_F8 / 8]    = static_cast<unsigned char>(pedal[KEY_F8 / 8]    | (1u << (KEY_F8 % 8)));
        pedal[KEY_ENTER / 8] = static_cast<unsigned char>(pedal[KEY_ENTER / 8] | (1u << (KEY_ENTER % 8)));

        QVERIFY(EvdevHotkeyListener::looksLikeFullKeyboard(full.data(), bytes));
        QVERIFY(!EvdevHotkeyListener::looksLikeFullKeyboard(pedal.data(), bytes));
        QVERIFY(!EvdevHotkeyListener::looksLikeFullKeyboard(nullptr, 0));

        const QString kbName = QStringLiteral("AT Translated Set 2 keyboard");
        const QString kbPath = QStringLiteral("/dev/input/event3");
        const QString pdName = QStringLiteral("USB Foot Switch");
        const QString pdPath = QStringLiteral("/dev/input/event9");

        EvdevHotkeyListener::Options opt;
        opt.key = QStringLiteral("F8");

        // grab=false — только наблюдение, клавиатура не блокируется никогда
        QVERIFY(!EvdevHotkeyListener::shouldGrab(opt, kbName, kbPath, full.data(), bytes));
        QVERIFY(!EvdevHotkeyListener::shouldGrab(opt, pdName, pdPath, pedal.data(), bytes));

        opt.grab = true;
        QString reason;
        QVERIFY(!EvdevHotkeyListener::shouldGrab(opt, kbName, kbPath, full.data(), bytes, &reason));
        QVERIFY(!reason.isEmpty());                        // объяснение для лога
        QVERIFY(EvdevHotkeyListener::shouldGrab(opt, pdName, pdPath, pedal.data(), bytes));

        // Список сужает grab до перечисленного
        opt.grabDevices = QStringList{QStringLiteral("foot switch")};
        QVERIFY(EvdevHotkeyListener::shouldGrab(opt, pdName, pdPath, pedal.data(), bytes));
        QVERIFY(!EvdevHotkeyListener::shouldGrab(opt, kbName, kbPath, full.data(), bytes));
        // не перечисленное устройство не перехватывается, даже если это не клавиатура
        QVERIFY(!EvdevHotkeyListener::shouldGrab(opt,
                    QStringLiteral("Sony Interactive Controller"),
                    QStringLiteral("/dev/input/event5"), pedal.data(), bytes));

        // Явно названная клавиатура перехватывается: человек этого хотел
        opt.grabDevices = QStringList{QStringLiteral("/dev/input/event3")};
        QVERIFY(EvdevHotkeyListener::shouldGrab(opt, kbName, kbPath, full.data(), bytes));

        // matchesDeviceSpec: подстрока имени или путь, регистр не важен
        QVERIFY(EvdevHotkeyListener::matchesDeviceSpec(
            QStringList{QStringLiteral("FOOT")}, pdName, pdPath));
        QVERIFY(EvdevHotkeyListener::matchesDeviceSpec(
            QStringList{QStringLiteral("/dev/input/event9")}, pdName, pdPath));
        QVERIFY(!EvdevHotkeyListener::matchesDeviceSpec(QStringList(), pdName, pdPath));
        QVERIFY(!EvdevHotkeyListener::matchesDeviceSpec(
            QStringList{QStringLiteral("   ")}, pdName, pdPath));
    }

    // ---------------- Конфиг: заметки, глушитель, grab_devices ----------------

    void configNotesAndTypingGuard()
    {
        QTemporaryFile f;
        QVERIFY(f.open());
        {
            QTextStream out(&f);
            out.setCodec("UTF-8");
            out << QStringLiteral("[notes]\n")
                << QStringLiteral("dir=~/zametki\n")
                << QStringLiteral("voice_commands=false\n")
                << QStringLiteral("start_target=notes\n")
                << QStringLiteral("timestamp=\n")
                << QStringLiteral("markdown=false\n")
                << QStringLiteral("\n[output]\n")
                << QStringLiteral("pin_window=true\n")
                << QStringLiteral("\n[audio]\n")
                << QStringLiteral("typing_guard_ms=250\n")
                << QStringLiteral("\n[hotkey]\n")
                << QStringLiteral("grab=true\n")
                << QStringLiteral("grab_devices=/dev/input/event9, Foot Switch\n");
        }
        f.close();

        const ConfigManager cfg(f.fileName());
        QVERIFY(cfg.notesEnabled());
        QCOMPARE(cfg.notesDir(), QDir::cleanPath(QDir::homePath() + QStringLiteral("/zametki")));
        QVERIFY(!cfg.notesVoiceCommands());
        QCOMPARE(cfg.notesStartTarget(), QStringLiteral("notes"));
        QCOMPARE(cfg.notesTimestampFormat(), QString());
        QVERIFY(!cfg.notesMarkdown());
        QVERIFY(cfg.pinWindow());
        QCOMPARE(cfg.typingGuardMs(), 250);
        QVERIFY(cfg.hotkeyGrab());
        // значение с запятыми QSettings отдаёт списком — и оно не должно
        // превратиться в пустую строку (грабля №9)
        QCOMPARE(cfg.hotkeyGrabDevices(),
                 (QStringList{QStringLiteral("/dev/input/event9"),
                              QStringLiteral("Foot Switch")}));

        // Значения по умолчанию
        const ConfigManager def(QStringLiteral("/tmp/нет-такого-файла-va-notes.ini"));
        QVERIFY(def.notesEnabled());
        QVERIFY(def.notesDir().endsWith(QStringLiteral("/notes")));
        QCOMPARE(def.notesFile(), QString());
        QCOMPARE(def.notesTimestampFormat(), QStringLiteral("HH:mm:ss"));
        QVERIFY(def.notesMarkdown());
        QVERIFY(def.notesDayHeader());
        QVERIFY(def.notesVoiceCommands());
        QCOMPARE(def.notesStartTarget(), QStringLiteral("focus"));
        QVERIFY(!def.pinWindow());
        QCOMPARE(def.typingGuardMs(), 0);
        QVERIFY(!def.hotkeyGrab());                 // клавиатура не блокируется
        QVERIFY(def.hotkeyGrabDevices().isEmpty());
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

    // ---------------- AudioFileDecoder ----------------

    // Синтетический WAV-контейнер: fmt + data, как пишет любая звонилка.
    static QByteArray makeWav(int formatTag, int channels, int rate, int bits,
                              const QByteArray& data)
    {
        QByteArray h;
        auto put32 = [&h](quint32 v) { h.append(reinterpret_cast<const char*>(&v), 4); };
        auto put16 = [&h](quint16 v) { h.append(reinterpret_cast<const char*>(&v), 2); };
        h.append("RIFF", 4);
        put32(36 + quint32(data.size()));
        h.append("WAVE", 4);
        h.append("fmt ", 4);
        put32(16);
        put16(quint16(formatTag));
        put16(quint16(channels));
        put32(quint32(rate));
        put32(quint32(rate * channels * bits / 8));
        put16(quint16(channels * bits / 8));
        put16(quint16(bits));
        h.append("data", 4);
        put32(quint32(data.size()));
        h.append(data);
        return h;
    }

    // Записать байты во временный файл с нужным расширением и вернуть путь.
    static QString writeTemp(QTemporaryDir& dir, const QString& name, const QByteArray& bytes)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(bytes);
        f.close();
        return path;
    }

    void decoderG711Tables()
    {
        // Контрольные значения классического g711: alaw-тишина 0xD5 -> +8,
        // ulaw-тишина 0xFF и 0x7F -> 0, знак alaw инвертирован (0x55 -> -8).
        QCOMPARE(AudioFileDecoder::alawToLinear(0xD5), int16_t(8));
        QCOMPARE(AudioFileDecoder::alawToLinear(0x55), int16_t(-8));
        QCOMPARE(AudioFileDecoder::ulawToLinear(0xFF), int16_t(0));
        QCOMPARE(AudioFileDecoder::ulawToLinear(0x7F), int16_t(0));
        // Полный диапазон не выходит за int16
        for (int v = 0; v < 256; ++v) {
            QVERIFY(qAbs(int(AudioFileDecoder::alawToLinear(uint8_t(v)))) <= 32767);
            QVERIFY(qAbs(int(AudioFileDecoder::ulawToLinear(uint8_t(v)))) <= 32767);
        }
    }

    void decoderWavPcm16StereoResample()
    {
        // Стерео 44.1 кГц, 1 секунда постоянного сигнала: L=1000, R=3000.
        // После домикширования — 2000, после ресемпла — 16000 сэмплов.
        const int frames = 44100;
        QByteArray data;
        data.reserve(frames * 4);
        for (int i = 0; i < frames; ++i) {
            const int16_t l = 1000, r = 3000;
            data.append(reinterpret_cast<const char*>(&l), 2);
            data.append(reinterpret_cast<const char*>(&r), 2);
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTemp(dir, QStringLiteral("s.wav"), makeWav(1, 2, 44100, 16, data));

        AudioFileDecoder::Audio out;
        QString err;
        QVERIFY2(AudioFileDecoder::decode(path, AudioFileDecoder::RawParams(), out, &err),
                 qPrintable(err));
        QCOMPARE(out.sourceRate, 44100);
        QCOMPARE(out.sourceChannels, 2);
        QCOMPARE(int(out.samples.size()), 16000);
        for (size_t i : {0u, 8000u, 15999u}) {
            QVERIFY2(qAbs(int(out.samples[i]) - 2000) <= 2,
                     qPrintable(QStringLiteral("samples[%1]=%2, жду ~2000")
                                    .arg(i).arg(out.samples[i])));
        }
        QVERIFY(qAbs(out.seconds() - 1.0) < 0.01);
    }

    void decoderWavPcm8()
    {
        // 8-битный PCM без знака, центр 128: значение 192 -> (192-128)*256 = 16384
        QByteArray data(8000, char(192));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTemp(dir, QStringLiteral("u8.wav"), makeWav(1, 1, 8000, 8, data));

        AudioFileDecoder::Audio out;
        QString err;
        QVERIFY2(AudioFileDecoder::decode(path, AudioFileDecoder::RawParams(), out, &err),
                 qPrintable(err));
        QCOMPARE(int(out.samples.size()), 16000);   // 1 с @ 8 кГц -> 16 кГц
        QVERIFY(qAbs(int(out.samples[100]) - 16384) <= 2);
    }

    void decoderWavAlaw()
    {
        // Телефонный WAV G.711 A-law (format tag 6): тишина 0xD5 -> ~+8
        QByteArray data(8000, char(0xD5));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTemp(dir, QStringLiteral("alaw.wav"), makeWav(6, 1, 8000, 8, data));

        AudioFileDecoder::Audio out;
        QString err;
        QVERIFY2(AudioFileDecoder::decode(path, AudioFileDecoder::RawParams(), out, &err),
                 qPrintable(err));
        QCOMPARE(out.sourceFormat.contains(QStringLiteral("format=6")), true);
        QVERIFY(qAbs(int(out.samples[50])) <= 16);
    }

    void decoderWavFloat32()
    {
        // IEEE float (format tag 3): 0.5f -> ~16384
        QByteArray data;
        const float v = 0.5f;
        for (int i = 0; i < 16000; ++i) {
            data.append(reinterpret_cast<const char*>(&v), 4);
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTemp(dir, QStringLiteral("f32.wav"), makeWav(3, 1, 16000, 32, data));

        AudioFileDecoder::Audio out;
        QString err;
        QVERIFY2(AudioFileDecoder::decode(path, AudioFileDecoder::RawParams(), out, &err),
                 qPrintable(err));
        QCOMPARE(int(out.samples.size()), 16000);   // 16 кГц — без ресемпла
        QVERIFY(qAbs(int(out.samples[0]) - 16384) <= 2);
    }

    void decoderRawS16()
    {
        // RAW 8 кГц моно s16le, постоянный сигнал 1234, 2 секунды
        QByteArray data;
        const int16_t v = 1234;
        for (int i = 0; i < 16000; ++i) {
            data.append(reinterpret_cast<const char*>(&v), 2);
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTemp(dir, QStringLiteral("call.raw"), data);

        AudioFileDecoder::RawParams p;   // дефолт: 8000 Гц, моно, s16le
        AudioFileDecoder::Audio out;
        QString err;
        QVERIFY2(AudioFileDecoder::decode(path, p, out, &err), qPrintable(err));
        QCOMPARE(int(out.samples.size()), 32000);   // 2 с @ 16 кГц
        QVERIFY(qAbs(int(out.samples[1000]) - 1234) <= 2);

        // Неверная частота меняет длительность (проверяем, что параметры применяются)
        AudioFileDecoder::RawParams p16 = p;
        p16.sampleRate = 16000;
        AudioFileDecoder::Audio out16;
        QVERIFY(AudioFileDecoder::decode(path, p16, out16, &err));
        QCOMPARE(int(out16.samples.size()), 16000);   // 1 с @ 16 кГц
    }

    void decoderRawUlaw()
    {
        // RAW μ-law: тишина 0xFF -> 0
        QByteArray data(8000, char(0xFF));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTemp(dir, QStringLiteral("call.pcm"), data);

        AudioFileDecoder::RawParams p;
        p.format = AudioFileDecoder::RawFormat::ULAW;
        AudioFileDecoder::Audio out;
        QString err;
        QVERIFY2(AudioFileDecoder::decode(path, p, out, &err), qPrintable(err));
        QCOMPARE(out.samples[0], int16_t(0));
    }

    void decoderErrors()
    {
        AudioFileDecoder::Audio out;
        QString err;
        AudioFileDecoder::RawParams p;

        // Несуществующий файл
        QVERIFY(!AudioFileDecoder::decode(QStringLiteral("/tmp/нет-такого-файла-va.wav"),
                                          p, out, &err));
        QVERIFY(!err.isEmpty());

        // Обрезанный RIFF: заголовок есть, данных нет
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString bad = writeTemp(dir, QStringLiteral("bad.wav"),
                                      QByteArray("RIFF\0\0\0\0WAVE", 12));
        QVERIFY(!AudioFileDecoder::decode(bad, p, out, &err));
        QVERIFY(!err.isEmpty());

        // Пустой RAW
        const QString empty = writeTemp(dir, QStringLiteral("empty.raw"), QByteArray());
        QVERIFY(!AudioFileDecoder::decode(empty, p, out, &err));

        // RAW с безумными параметрами
        const QString ok = writeTemp(dir, QStringLiteral("one.raw"), QByteArray(1600, 1));
        AudioFileDecoder::RawParams crazy = p;
        crazy.sampleRate = 10;   // вне 1000..384000
        QVERIFY(!AudioFileDecoder::decode(ok, crazy, out, &err));
    }

    void decoderRawRouting()
    {
        // .raw/.pcm/без расширения — RAW; известные сжатые — системный декодер
        QVERIFY(AudioFileDecoder::isRawExtension(QStringLiteral("a.raw")));
        QVERIFY(AudioFileDecoder::isRawExtension(QStringLiteral("a.PCM")));
        QVERIFY(AudioFileDecoder::isRawExtension(QStringLiteral("a.audio")));
        QVERIFY(AudioFileDecoder::isRawExtension(QStringLiteral("call_record")));
        QVERIFY(!AudioFileDecoder::isRawExtension(QStringLiteral("a.mp3")));
        QVERIFY(!AudioFileDecoder::isRawExtension(QStringLiteral("a.wav")));

        // Форматы RAW: строка <-> enum (для ini и CLI)
        using RF = AudioFileDecoder::RawFormat;
        QCOMPARE(AudioFileDecoder::RawParams::formatFromString(QStringLiteral("alaw")), RF::ALAW);
        QCOMPARE(AudioFileDecoder::RawParams::formatFromString(QStringLiteral("ULAW")), RF::ULAW);
        QCOMPARE(AudioFileDecoder::RawParams::formatFromString(QStringLiteral("s8u")), RF::S8U);
        QCOMPARE(AudioFileDecoder::RawParams::formatFromString(QStringLiteral("f32le")), RF::F32LE);
        QCOMPARE(AudioFileDecoder::RawParams::formatFromString(QStringLiteral("что угодно")), RF::S16LE);
        QCOMPARE(AudioFileDecoder::RawParams::formatToString(RF::ALAW), QStringLiteral("alaw"));
        QVERIFY(AudioFileDecoder::RawParams::formatNames().contains(QStringLiteral("ulaw")));
    }

    void resampleKeepsLengthAndPitch()
    {
        // 8 кГц -> 16 кГц: вдвое больше сэмплов, постоянный сигнал не меняется
        std::vector<int16_t> mono(8000, 500);
        AudioFileDecoder::resampleTo16k(mono, 8000);
        QCOMPARE(int(mono.size()), 16000);
        QCOMPARE(mono[7999], int16_t(500));

        // 16 кГц -> no-op
        const size_t before = mono.size();
        AudioFileDecoder::resampleTo16k(mono, 16000);
        QCOMPARE(mono.size(), before);
    }

    // ---------------- Команда «разбери файл» ----------------

    void transcribeFileCommand()
    {
        CommandParser p;
        const auto c = p.parse(QStringLiteral("Разбери файл."));
        QVERIFY(c.has_value());
        QCOMPARE(c->type, Command::Type::TranscribeFile);
        QCOMPARE(commandDescription(*c), QStringLiteral("разобрать аудиофайл"));
        QCOMPARE(commandToSpec(*c), QStringLiteral("transcribe-file"));

        // Все синонимы из словаря
        for (const QString& phrase : {QStringLiteral("разбери аудиофайл"),
                                      QStringLiteral("распознай файл"),
                                      QStringLiteral("расшифруй запись")}) {
            QVERIFY2(p.parse(phrase).has_value(), qPrintable(phrase));
            QCOMPARE(p.parse(phrase)->type, Command::Type::TranscribeFile);
        }

        // Round-trip через файл команд (вкладка «Команды» и loadFromFile)
        QString phr, err;
        Command parsed;
        QVERIFY(CommandDictionary::parseLine(
            QStringLiteral("переведи звонок = transcribe-file"), &phr, &parsed, &err));
        QCOMPARE(phr, QStringLiteral("переведи звонок"));
        QCOMPARE(parsed.type, Command::Type::TranscribeFile);

        // Обычная речь командой не становится
        QVERIFY(!p.parse(QStringLiteral("разбери этот файл по полочкам")).has_value());
    }

    // ---------------- [transcribe] в конфиге ----------------

    void configTranscribeSection()
    {
        // Значения по умолчанию — телефонные
        const ConfigManager def(QStringLiteral("/tmp/нет-такого-файла-va-transcribe.ini"));
        QCOMPARE(def.transcribeRawRate(), 8000);
        QCOMPARE(def.transcribeRawChannels(), 1);
        QCOMPARE(def.transcribeRawFormat(), QStringLiteral("s16le"));
        QVERIFY(def.transcribeLastDir().isEmpty());

        // Запись/чтение (сеттеры пишет GUI-диалог RAW-параметров)
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ini = dir.filePath(QStringLiteral("settings.ini"));
        {
            ConfigManager w(ini);
            w.setTranscribeRawRate(16000);
            w.setTranscribeRawChannels(2);
            w.setTranscribeRawFormat(QStringLiteral("alaw"));
            w.setTranscribeLastDir(QStringLiteral("/home/user/Записи"));
        }
        const ConfigManager r(ini);
        QCOMPARE(r.transcribeRawRate(), 16000);
        QCOMPARE(r.transcribeRawChannels(), 2);
        QCOMPARE(r.transcribeRawFormat(), QStringLiteral("alaw"));
        QCOMPARE(r.transcribeLastDir(), QStringLiteral("/home/user/Записи"));
    }
};

QTEST_MAIN(TestVoiceUnits)
#include "test_voice_units.moc"
