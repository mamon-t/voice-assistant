#include "CommandParser.h"

CommandParser::CommandParser(QObject* parent)
    : QObject(parent)
{
}

CommandParser::~CommandParser() = default;

std::optional<Command> CommandParser::parse(const QString& text) {
    Q_UNUSED(text)
    // TODO: парсинг команд
    return std::nullopt;
}