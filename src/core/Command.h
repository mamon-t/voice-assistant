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