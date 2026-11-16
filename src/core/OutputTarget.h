#pragma once

#include <QString>

// Куда класть распознанный текст.
//
// Focus — в окно, которое в фокусе (обычная диктовка: xdotool type или
//         буфер обмена + Ctrl+V).
// Notes — в файл заметок (FileInjector). Клавиатура, фокус и буфер обмена не
//         трогаются вообще, поэтому заметки можно диктовать, продолжая печатать
//         руками в другом окне.
//
// Цель намеренно ортогональна режиму (Mode): режим описывает, ЧТО делать
// с речью (диктовать / править / проверять орфографию), цель — КУДА класть
// результат. Заметки можно диктовать и проверять на орфографию одновременно.
enum class OutputTarget {
    Focus,
    Notes
};

inline QString outputTargetToString(OutputTarget target)
{
    switch (target) {
        case OutputTarget::Focus: return QStringLiteral("focus");
        case OutputTarget::Notes: return QStringLiteral("notes");
    }
    return QStringLiteral("focus");
}

// Принимает и английские, и русские имена — их произносят голосом и пишут
// в D-Bus-вызовах одинаково часто.
inline bool tryParseOutputTarget(const QString& str, OutputTarget* out)
{
    const QString v = str.trimmed().toLower();
    if (v == QLatin1String("focus") || v == QLatin1String("window")
        || v == QLatin1String("active")
        || v == QStringLiteral("окно") || v == QStringLiteral("редактор")
        || v == QStringLiteral("фокус")) {
        if (out) *out = OutputTarget::Focus;
        return true;
    }
    // Кириллические имена сравниваются через QStringLiteral, НЕ через
    // QLatin1String: исходник в UTF-8, а QLatin1String трактует каждый байт
    // как отдельный символ Latin-1, и «заметки» никогда не совпадёт
    // (грабля №6 в development-log.md — тест это поймал).
    if (v == QLatin1String("notes") || v == QLatin1String("note")
        || v == QLatin1String("file")
        || v == QStringLiteral("заметки") || v == QStringLiteral("заметка")) {
        if (out) *out = OutputTarget::Notes;
        return true;
    }
    return false;
}

inline OutputTarget stringToOutputTarget(const QString& str)
{
    OutputTarget t = OutputTarget::Focus;
    tryParseOutputTarget(str, &t);
    return t;
}

// Человекочитаемое имя — для логов, трея и уведомлений.
inline QString outputTargetTitle(OutputTarget target)
{
    return target == OutputTarget::Notes ? QStringLiteral("файл заметок")
                                        : QStringLiteral("активное окно");
}
