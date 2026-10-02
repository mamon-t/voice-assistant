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
    } else {
        return false;
    }
    return true;
}

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
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            qWarning().noquote() << QString("CommandDictionary: %1:%2 — жду 'фраза = тип[:аргумент]'")
                                        .arg(path).arg(lineNo);
            continue;
        }
        const QString phrase = line.left(eq).trimmed();
        const QString spec   = line.mid(eq + 1).trimmed().toLower();

        Command cmd;
        if (!parseCommandSpec(spec, &cmd)) {
            qWarning().noquote() << QString("CommandDictionary: %1:%2 — неизвестный тип '%3'")
                                        .arg(path).arg(lineNo).arg(spec);
            continue;
        }
        addCommand(phrase, cmd);
        ++added;
    }

    qDebug().noquote() << QString("CommandDictionary: +%1 команд из %2").arg(added).arg(path);
    return added;
}

void CommandDictionary::addCommand(const QString& phrase, Command command)
{
    const QString k = key(phrase);
    if (k.isEmpty()) {
        return;
    }
    m_commands.insert(k, command);
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
