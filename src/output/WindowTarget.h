#pragma once

#include <QString>
#include <QStringList>

// ---------------------------------------------------------------------------
// Куда именно вставлять текст, когда включён [output] pin_window.
// ---------------------------------------------------------------------------
//
// ЗАЧЕМ ЭТОТ ФАЙЛ. Привязка вставки к окну («диктую в редактор, а сам в это
// время смотрю в браузер») упиралась в то, что `xdotool … --window WID`
// доставляет события через XSendEvent. Такие события помечены флагом
// send_event=1, и значительная часть приложений их отбрасывает: браузеры,
// LibreOffice, Java/Eclipse, часть терминалов. При этом xdotool возвращает
// код 0 — в логе всё «успешно», а в редакторе пусто. Именно так выглядел баг:
// «лог виден, а не напечатал вообще ничего».
//
// Отсюда два честных способа доставки, и выбор между ними — чистая логика,
// которую можно проверить тестами без X-сервера. Ниже только она: ни xdotool,
// ни QProcess, ни задержек — решение принимается по трём строкам и двум флагам.
//
//   sendevent — xdotool … --window WID. Фокус НЕ трогается, можно печатать
//               руками в другом окне прямо во время диктовки. Работает, пока
//               приложение принимает синтетические события (Qt, GTK — да;
//               браузеры и LibreOffice — как повезёт).
//   activate  — окно сначала активируется (xdotool windowactivate), события
//               идут настоящими (XTest), затем фокус возвращается обратно.
//               Работает в ЛЮБОМ приложении, но на 50–150 мс забирает фокус.
//
// Что именно делает инжектор по этому решению — см. output/XdotoolInjector.cpp.

// Способ доставки и окно, в которое пойдёт текст.
struct WindowTarget {
    enum class Delivery {
        ActiveWindow,  // привязки нет — печатаем туда, где фокус сейчас
        SendEvent,     // xdotool … --window WID (синтетические события)
        Activate       // активировать окно, напечатать настоящими, вернуть фокус
    };

    Delivery delivery = Delivery::ActiveWindow;
    QString  window;        // WID цели; пуста при Delivery::ActiveWindow
    QString  restoreTo;     // куда вернуть фокус после Activate (пуста = не возвращать)

    bool usesWindowFlag() const { return delivery == Delivery::SendEvent; }
    bool needsActivation() const { return delivery == Delivery::Activate; }
};

// Режим привязки — как его задаёт пользователь в [output] pin_mode.
enum class PinMode {
    Activate,     // надёжно: активация + настоящие события (по умолчанию)
    SendEvent     // как раньше: --window, фокус не трогаем
};

inline QString pinModeToString(PinMode mode)
{
    return mode == PinMode::SendEvent ? QStringLiteral("sendevent")
                                      : QStringLiteral("activate");
}

// Принимает и «sendevent», и «send_event», и «--window» — так опцию проще
// понять из чужого конфига. Всё неизвестное трактуется как activate: это
// единственный способ, который действительно печатает в любом приложении,
// а тихий откат к нерабочему варианту мы уже проходили.
inline PinMode stringToPinMode(const QString& str)
{
    const QString v = str.trimmed().toLower();
    if (v == QLatin1String("sendevent") || v == QLatin1String("send_event")
        || v == QLatin1String("window") || v == QLatin1String("--window")
        || v == QLatin1String("xsendevent")) {
        return PinMode::SendEvent;
    }
    return PinMode::Activate;
}

// Название способа — для логов. «синтетика» против «настоящие события» важнее
// технических терминов: по логу должно быть видно, почему текст не дошёл.
inline QString deliveryToString(WindowTarget::Delivery d)
{
    switch (d) {
    case WindowTarget::Delivery::ActiveWindow: return QStringLiteral("активное окно");
    case WindowTarget::Delivery::SendEvent:    return QStringLiteral("синтетика (--window)");
    case WindowTarget::Delivery::Activate:     return QStringLiteral("активация + настоящие события");
    }
    return QStringLiteral("активное окно");
}

// ---------------------------------------------------------------------------
// Главное решение.
//
// pinned          — WID окна, запомненного в начале записи (пуста = привязки нет)
// active          — WID окна, которое в фокусе СЕЙЧАС (пуста = неизвестно)
// pinnedAlive     — существует ли окно pinned (false — его закрыли или это
//                   всплывающее меню трея, которое уже исчезло)
// mode            — [output] pin_mode
// allowRestore    — [output] pin_restore_focus: возвращать ли фокус обратно
// selfWindows     — WM_CLASS окон самого помощника: в них вставлять нельзя,
//                   иначе текст уходит в меню трея и пропадает вместе с ним
// ---------------------------------------------------------------------------
inline WindowTarget decideWindowTarget(const QString& pinned,
                                       const QString& active,
                                       bool pinnedAlive,
                                       PinMode mode,
                                       bool allowRestore,
                                       const QStringList& selfWindows = QStringList())
{
    WindowTarget t;

    const QString p = pinned.trimmed();
    const QString a = active.trimmed();

    // Привязки нет — печатаем туда, где фокус. Обычная диктовка.
    if (p.isEmpty()) {
        t.delivery = WindowTarget::Delivery::ActiveWindow;
        return t;
    }

    // Окна закрыто (или это уже исчезнувшее всплывающее меню). Печатать в него
    // бессмысленно: xdotool вернёт BadWindow, а текст пропадёт молча.
    // Откатываемся на активное окно — лучше вставка не туда, чем никуда.
    if (!pinnedAlive) {
        t.delivery = WindowTarget::Delivery::ActiveWindow;
        return t;
    }

    // Цель — окно самого помощника (настройки, редактор подсказок, меню трея).
    // Вставка туда — это всегда потерянный текст, поэтому привязку игнорируем.
    if (selfWindows.contains(p)) {
        t.delivery = WindowTarget::Delivery::ActiveWindow;
        return t;
    }

    // Окно и так в фокусе: xdotool сам пошлёт настоящие события (внутри него
    // есть проверка «если целевое окно == сфокусированное, берём XTest»),
    // поэтому активация и возврат фокуса не нужны.
    if (!a.isEmpty() && a == p) {
        t.delivery   = WindowTarget::Delivery::SendEvent;
        t.window     = p;
        return t;
    }

    // Обычный случай: фокус уже в другом окне.
    t.window = p;
    if (mode == PinMode::SendEvent) {
        t.delivery = WindowTarget::Delivery::SendEvent;
        return t;
    }
    t.delivery   = WindowTarget::Delivery::Activate;
    t.restoreTo  = (allowRestore && !a.isEmpty() && a != p) ? a : QString();
    return t;
}
