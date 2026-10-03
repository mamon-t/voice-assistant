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
inline OutputTarget stringToOutputTarget(const QString& str)
{
    const QString v = str.trimmed().toLower();
    // Кириллические имена сравниваются через QStringLiteral, НЕ через
    // QLatin1String: исходник в UTF-8, а QLatin1String трактует каждый байт
    // как отдельный символ Latin-1, и «заметки» никогда не совпадёт
    // (грабля №6 в development-log.md — тест это поймал).
    if (v == QLatin1String("notes") || v == QLatin1String("note")
        || v == QLatin1String("file")
        || v == QStringLiteral("заметки") || v == QStringLiteral("заметка")) {
        return OutputTarget::Notes;
    }
    return OutputTarget::Focus;
}

// Человекочитаемое имя — для логов, трея и уведомлений.
inline QString outputTargetTitle(OutputTarget target)
{
    return target == OutputTarget::Notes ? QStringLiteral("файл заметок")
                                        : QStringLiteral("активное окно");
}
