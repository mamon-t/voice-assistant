#pragma once

#include <QString>
#include <optional>
#include "core/Mode.h"
#include "core/OutputTarget.h"

struct Command {
    enum class Type {
        SetMode,
        SetTarget,    // куда писать результат: окно или файл заметок
        DeleteWord,
        DeleteLine,
        NewLine,
        Space,
        Punctuation,  // точка, запятая и т.д.
        Unknown
    };

    Type type = Type::Unknown;
    Mode mode = Mode::Off;           // для SetMode
    OutputTarget target = OutputTarget::Focus;   // для SetTarget
    QString argument;                // для Punctuation (какой символ)

    static Command setMode(Mode m) {
        Command cmd;
        cmd.type = Type::SetMode;
        cmd.mode = m;
        return cmd;
    }

    static Command setTarget(OutputTarget t) {
        Command cmd;
        cmd.type = Type::SetTarget;
        cmd.target = t;
        return cmd;
    }

    static Command deleteWord() {
        Command cmd;
        cmd.type = Type::DeleteWord;
        return cmd;
    }
    
    static Command deleteLine() {
        Command cmd;
        cmd.type = Type::DeleteLine;
        return cmd;
    }
    
    static Command newLine() {
        Command cmd;
        cmd.type = Type::NewLine;
        return cmd;
    }
    
    static Command space() {
        Command cmd;
        cmd.type = Type::Space;
        return cmd;
    }
    
    static Command punctuation(const QString& symbol) {
        Command cmd;
        cmd.type = Type::Punctuation;
        cmd.argument = symbol;
        return cmd;
    }
};

// Человекочитаемое описание команды — для логов, трея и вкладки «Команды»
// в настройках. Живёт здесь, а не в ApplicationController, потому что
// показывать «фраза -> действие» должен и UI.
inline QString commandDescription(const Command& cmd)
{
    switch (cmd.type) {
    case Command::Type::SetMode:     return QStringLiteral("режим: %1").arg(modeToString(cmd.mode));
    case Command::Type::SetTarget:   return QStringLiteral("куда писать: %1")
                                               .arg(outputTargetTitle(cmd.target));
    case Command::Type::DeleteWord:  return QStringLiteral("удалить слово");
    case Command::Type::DeleteLine:  return QStringLiteral("удалить строку");
    case Command::Type::NewLine:     return QStringLiteral("новая строка");
    case Command::Type::Space:       return QStringLiteral("пробел");
    case Command::Type::Punctuation: return QStringLiteral("знак: %1").arg(cmd.argument);
    case Command::Type::Unknown:     break;
    }
    return QStringLiteral("неизвестная команда");
}

// Обратная сериализация в формат своего файла команд
// («фраза = тип[:аргумент]», см. CommandDictionary::loadFromFile).
// Нужна кнопке «скопировать в свои» во вкладке «Команды».
// Для Unknown — пустая строка.
inline QString commandToSpec(const Command& cmd)
{
    switch (cmd.type) {
    case Command::Type::SetMode:
        switch (cmd.mode) {
        case Mode::Dictation:  return QStringLiteral("set-mode:dictation");
        case Mode::Edit:       return QStringLiteral("set-mode:edit");
        case Mode::Spellcheck: return QStringLiteral("set-mode:spellcheck");
        case Mode::Off:        return QStringLiteral("set-mode:off");
        case Mode::Error:      return QString();
        }
        return QString();
    case Command::Type::SetTarget:
        return cmd.target == OutputTarget::Notes ? QStringLiteral("set-target:notes")
                                                 : QStringLiteral("set-target:focus");
    case Command::Type::DeleteWord:  return QStringLiteral("delete-word");
    case Command::Type::DeleteLine:  return QStringLiteral("delete-line");
    case Command::Type::NewLine:     return QStringLiteral("new-line");
    case Command::Type::Space:       return QStringLiteral("space");
    case Command::Type::Punctuation: return QStringLiteral("punctuation:%1").arg(cmd.argument);
    case Command::Type::Unknown:     break;
    }
    return QString();
}