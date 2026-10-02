#include "CommandParser.h"

#include "commands/CommandDictionary.h"

#include <QStringList>

CommandParser::CommandParser(QObject* parent)
    : QObject(parent)
    , m_dictionary(std::make_unique<CommandDictionary>(this))
{
    m_dictionary->loadDefaults();
}

CommandParser::~CommandParser() = default;

std::optional<Command> CommandParser::parse(const QString& text)
{
    const QString normalized = normalize(text);
    if (normalized.isEmpty()) {
        return std::nullopt;
    }
    return m_dictionary->find(normalized);
}

CommandDictionary* CommandParser::dictionary()
{
    return m_dictionary.get();
}

const CommandDictionary* CommandParser::dictionary() const
{
    return m_dictionary.get();
}

QString CommandParser::normalize(const QString& text)
{
    QString s = text.trimmed().toLower();

    // Дефис и тире — РАЗДЕЛИТЕЛИ, а не мусор: «режим-правки» должно совпадать
    // с «режим правки». Если их просто удалить, получится «режимправки».
    static const QString dashes = QStringLiteral("-\u2014\u2013");   // дефис, em dash, en dash
    for (const QChar& d : dashes) {
        s.replace(d, QLatin1Char(' '));
    }

    // Убираем остальные знаки препинания: zipformer их и так не ставит, а Whisper
    // ставит, и «Удали слово.» должно разбираться так же, как «удали слово».
    static const QString punct = QStringLiteral(".,!?;:()[]{}«»\"'`");
    for (const QChar& c : punct) {
        s.remove(c);
    }

    const QStringList parts = s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    return parts.join(QLatin1Char(' '));
}
