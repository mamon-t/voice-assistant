#include "commands/CommandDictionary.h"

#include "commands/CommandParser.h"

#include <QFile>
#include <QTextStream>
#include <QDebug>

namespace {

// Ключ словаря = нормализованная фраза. Нормализация одна на всех
// (CommandParser::normalize), иначе словарь и парсер разъедутся.
QString key(const QString& phrase)
{
    return CommandParser::normalize(phrase);
}

bool parseCommandSpec(const QString& spec, Command* out)
{
    // "set-mode:edit", "delete-word", "punctuation:."
    const int colon = spec.indexOf(QLatin1Char(':'));
    const QString type = (colon < 0) ? spec : spec.left(colon);
    const QString arg  = (colon < 0) ? QString() : spec.mid(colon + 1);

    if (type == QLatin1String("set-mode")) {
        Mode m = Mode::Off;
        if      (arg == QLatin1String("dictation"))  m = Mode::Dictation;
        else if (arg == QLatin1String("edit"))       m = Mode::Edit;
        else if (arg == QLatin1String("spellcheck")) m = Mode::Spellcheck;
        else if (arg == QLatin1String("off"))        m = Mode::Off;
        else return false;
        *out = Command::setMode(m);
    } else if (type == QLatin1String("set-target")) {
        // "set-target:notes" / "set-target:focus"
        if (arg.isEmpty()) return false;
        *out = Command::setTarget(stringToOutputTarget(arg));
    } else if (type == QLatin1String("delete-word")) {
        *out = Command::deleteWord();
    } else if (type == QLatin1String("delete-line")) {
        *out = Command::deleteLine();
    } else if (type == QLatin1String("new-line")) {
        *out = Command::newLine();
    } else if (type == QLatin1String("space")) {
        *out = Command::space();
    } else if (type == QLatin1String("punctuation")) {
        if (arg.isEmpty()) return false;
        *out = Command::punctuation(arg);
    } else if (type == QLatin1String("transcribe-file")) {
        *out = Command::transcribeFile();
    } else {
        return false;
    }
    return true;
}

// Фраза -> цель. Таблица одна и для loadDefaults(), и для targetCommandPhrases(),
// чтобы список фраз не разъезжался с их назначением.
struct TargetPhrase {
    const char*  phrase;
    OutputTarget target;
};

const TargetPhrase kTargetPhrases[] = {
    { "заметка",         OutputTarget::Notes },
    { "в заметки",       OutputTarget::Notes },
    { "пиши в заметки",  OutputTarget::Notes },
    { "заметки",         OutputTarget::Notes },
    { "в редактор",      OutputTarget::Focus },
    { "в окно",          OutputTarget::Focus },
    { "диктовка в окно", OutputTarget::Focus },
};

}  // namespace

CommandDictionary::CommandDictionary(QObject* parent)
    : QObject(parent)
{
}

CommandDictionary::~CommandDictionary() = default;

void CommandDictionary::loadDefaults()
{
    // --- переключение режимов (работают в любом режиме) ---
    addCommand(QStringLiteral("режим редактирования"),   Command::setMode(Mode::Edit));
    addCommand(QStringLiteral("режим правки"),           Command::setMode(Mode::Edit));
    addCommand(QStringLiteral("режим ввода"),            Command::setMode(Mode::Dictation));
    addCommand(QStringLiteral("режим диктовки"),         Command::setMode(Mode::Dictation));
    addCommand(QStringLiteral("режим проверки"),         Command::setMode(Mode::Spellcheck));
    addCommand(QStringLiteral("режим правописания"),     Command::setMode(Mode::Spellcheck));
    addCommand(QStringLiteral("выключить"),              Command::setMode(Mode::Off));
    addCommand(QStringLiteral("стоп"),                   Command::setMode(Mode::Off));
    addCommand(QStringLiteral("режим выключен"),         Command::setMode(Mode::Off));

    // --- цель вывода: окно или файл заметок ---
    // Работают в любом режиме, включая диктовку: это не правка текста, а
    // маршрутизация, поэтому ApplicationController обрабатывает их ДО запрета
    // editing_in_dictation (иначе сказанное «заметка» в диктовке пропало бы).
    for (const TargetPhrase& t : kTargetPhrases) {
        addCommand(QString::fromUtf8(t.phrase), Command::setTarget(t.target));
    }

    // --- правка текста ---
    addCommand(QStringLiteral("удали слово"),            Command::deleteWord());
    addCommand(QStringLiteral("удали последнее слово"),  Command::deleteWord());
    addCommand(QStringLiteral("сотри слово"),            Command::deleteWord());
    addCommand(QStringLiteral("удали строку"),           Command::deleteLine());
    addCommand(QStringLiteral("удали последнюю строку"), Command::deleteLine());

    // --- вставка ---
    addCommand(QStringLiteral("новая строка"),           Command::newLine());
    addCommand(QStringLiteral("перенос строки"),         Command::newLine());
    addCommand(QStringLiteral("абзац"),                  Command::newLine());
    addCommand(QStringLiteral("пробел"),                 Command::space());

    // --- одиночные знаки (в режиме диктовки их заменяет TextPostProcessor,
    //     здесь они нужны для режима правки) ---
    addCommand(QStringLiteral("точка"),                  Command::punctuation(QStringLiteral(".")));
    addCommand(QStringLiteral("запятая"),                Command::punctuation(QStringLiteral(",")));
    addCommand(QStringLiteral("вопросительный знак"),    Command::punctuation(QStringLiteral("?")));
    addCommand(QStringLiteral("восклицательный знак"),   Command::punctuation(QStringLiteral("!")));
    addCommand(QStringLiteral("двоеточие"),              Command::punctuation(QStringLiteral(":")));
    addCommand(QStringLiteral("точка с запятой"),        Command::punctuation(QStringLiteral(";")));
    addCommand(QStringLiteral("тире"),                   Command::punctuation(QStringLiteral("—")));
    addCommand(QStringLiteral("дефис"),                  Command::punctuation(QStringLiteral("-")));

    // --- разбор аудиофайла ---
    // Как и смена режима/цели, это маршрутизация, а не правка текста: работает
    // в любом режиме, включая диктовку с editing_in_dictation=false. По команде
    // диктовка останавливается, UI открывает системный диалог выбора файла,
    // а расшифровка вставляется в окно, активное ДО диалога (или в заметки).
    addCommand(QStringLiteral("разбери файл"),           Command::transcribeFile());
    addCommand(QStringLiteral("разбери аудиофайл"),      Command::transcribeFile());
    addCommand(QStringLiteral("разбери запись"),         Command::transcribeFile());
    addCommand(QStringLiteral("распознай файл"),         Command::transcribeFile());
    addCommand(QStringLiteral("распознай аудиофайл"),    Command::transcribeFile());
    addCommand(QStringLiteral("расшифруй файл"),         Command::transcribeFile());
    addCommand(QStringLiteral("расшифруй аудиофайл"),    Command::transcribeFile());
    addCommand(QStringLiteral("расшифруй запись"),       Command::transcribeFile());
}

bool CommandDictionary::parseLine(const QString& line, QString* phrase, Command* cmd,
                                  QString* error)
{
    if (error) {
        error->clear();
    }
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
        return false;   // не ошибка — просто нечего добавлять
    }
    const int eq = trimmed.indexOf(QLatin1Char('='));
    if (eq <= 0) {
        if (error) {
            *error = QStringLiteral("жду 'фраза = тип[:аргумент]'");
        }
        return false;
    }
    const QString phr  = trimmed.left(eq).trimmed();
    const QString spec = trimmed.mid(eq + 1).trimmed().toLower();
    if (phr.isEmpty()) {
        if (error) {
            *error = QStringLiteral("пустая фраза слева от '='");
        }
        return false;
    }
    Command parsed;
    if (!parseCommandSpec(spec, &parsed)) {
        if (error) {
            *error = QStringLiteral("неизвестный тип '%1' (доступны: set-mode:{dictation|edit|"
                                    "spellcheck|off}, set-target:{focus|notes}, delete-word, "
                                    "delete-line, new-line, space, punctuation:<символ>, "
                                    "transcribe-file)").arg(spec);
        }
        return false;
    }
    if (phrase) {
        *phrase = phr;
    }
    if (cmd) {
        *cmd = parsed;
    }
    return true;
}

int CommandDictionary::loadFromFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }

    QTextStream in(&f);
    in.setCodec("UTF-8");

    int added = 0;
    int lineNo = 0;
    while (!in.atEnd()) {
        ++lineNo;
        const QString line = in.readLine();
        QString phrase;
        Command cmd;
        QString err;
        if (parseLine(line, &phrase, &cmd, &err)) {
            addCommand(phrase, cmd);
            ++added;
        } else if (!err.isEmpty()) {
            qWarning().noquote() << QString("CommandDictionary: %1:%2 — %3")
                                        .arg(path).arg(lineNo).arg(err);
        }
    }

    qDebug().noquote() << QString("CommandDictionary: +%1 команд из %2").arg(added).arg(path);
    return added;
}

QStringList CommandDictionary::targetCommandPhrases()
{
    QStringList out;
    out.reserve(static_cast<int>(sizeof(kTargetPhrases) / sizeof(kTargetPhrases[0])));
    for (const TargetPhrase& t : kTargetPhrases) {
        out << QString::fromUtf8(t.phrase);
    }
    return out;
}

void CommandDictionary::addCommand(const QString& phrase, Command command)
{
    const QString k = key(phrase);
    if (k.isEmpty()) {
        return;
    }
    m_commands.insert(k, command);
}

bool CommandDictionary::removeCommand(const QString& phrase)
{
    return m_commands.remove(key(phrase)) > 0;
}

void CommandDictionary::clear()
{
    m_commands.clear();
}

std::optional<Command> CommandDictionary::find(const QString& normalizedPhrase) const
{
    const auto it = m_commands.constFind(normalizedPhrase);
    if (it == m_commands.constEnd()) {
        return std::nullopt;
    }
    return *it;
}

bool CommandDictionary::contains(const QString& normalizedPhrase) const
{
    return m_commands.contains(normalizedPhrase);
}

QStringList CommandDictionary::phrases() const
{
    return m_commands.keys();
}

int CommandDictionary::size() const
{
    return m_commands.size();
}
