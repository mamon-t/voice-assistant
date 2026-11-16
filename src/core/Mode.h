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

inline bool tryParseMode(const QString& str, Mode* out)
{
    const QString v = str.trimmed().toLower();
    if (v == QLatin1String("off"))         { if (out) *out = Mode::Off;         return true; }
    if (v == QLatin1String("dictation"))   { if (out) *out = Mode::Dictation;   return true; }
    if (v == QLatin1String("edit"))        { if (out) *out = Mode::Edit;        return true; }
    if (v == QLatin1String("spellcheck"))  { if (out) *out = Mode::Spellcheck;  return true; }
    if (v == QLatin1String("error"))       { if (out) *out = Mode::Error;       return true; }
    return false;
}

inline Mode stringToMode(const QString& str) {
    Mode m = Mode::Off;
    tryParseMode(str, &m);
    return m;
}