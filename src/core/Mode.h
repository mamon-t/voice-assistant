#pragma once

#include <QString>

enum class Mode {
    Off,
    Dictation,
    Edit,
    Spellcheck,
    Error
};

inline QString modeToString(Mode mode) {
    switch (mode) {
        case Mode::Off:         return "off";
        case Mode::Dictation:   return "dictation";
        case Mode::Edit:        return "edit";
        case Mode::Spellcheck:  return "spellcheck";
        case Mode::Error:       return "error";
    }
    return "unknown";
}

inline Mode stringToMode(const QString& str) {
    if (str == "off")         return Mode::Off;
    if (str == "dictation")   return Mode::Dictation;
    if (str == "edit")        return Mode::Edit;
    if (str == "spellcheck")  return Mode::Spellcheck;
    if (str == "error")       return Mode::Error;
    return Mode::Off;
}