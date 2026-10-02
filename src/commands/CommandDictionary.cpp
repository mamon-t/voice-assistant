#include "commands/CommandDictionary.h"

CommandDictionary::CommandDictionary(QObject* parent)
    : QObject(parent)
{
}

CommandDictionary::~CommandDictionary() = default;

void CommandDictionary::loadDefaults() {
    // TODO: загрузить стандартные команды
}

void CommandDictionary::addCommand(const QString& phrase, Command command) {
    m_commands[phrase] = command;
}