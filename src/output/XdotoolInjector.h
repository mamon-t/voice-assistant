#pragma once

#include "output/ITextInjector.h"
#include "output/WindowTarget.h"

#include <QString>
#include <QStringList>

// Ввод текста и клавиш через xdotool (X11).
//
// Почему два способа ввода текста:
//   * `xdotool type` печатает посимвольно и для кириллицы ТРЕБУЕТ русской
//     раскладки в момент ввода: если активна английская, вместо «привет»
//     уедет «ghbdtn». Плюс это медленно (~12 мс на символ).
//   * буфер обмена (xclip/xsel/wl-copy) + Ctrl+V вставляет текст как есть,
//     независимо от раскладки, и мгновенно. Поэтому по умолчанию Auto:
//     если найден инструмент буфера обмена — печатаем через него.
//
// Для отладки без X-сервера: VOICE_ASSISTANT_DRYRUN=1 — все вызовы логируются
// и возвращают true, ничего не отправляя.
class XdotoolInjector : public ITextInjector {
public:
    enum class Method {
        Auto,          // буфер обмена, если есть; иначе xdotool type
        XdotoolType,   // всегда xdotool type (медленно, зависит от раскладки)
        Clipboard      // всегда буфер обмена + Ctrl+V
    };

    XdotoolInjector();
    ~XdotoolInjector();

    bool initialize() override;
    bool typeText(const QString& text) override;
    bool sendKey(const QString& key) override;   // "BackSpace", "Return", "ctrl+BackSpace"
    bool isAvailable() const override;

    void setMethod(Method method);
    Method method() const { return m_method; }
    void setTypingDelayMs(int ms);

    // Вставка идёт через буфер обмена, то есть затирает то, что там лежало.
    // Если включено — прежний ТЕКСТ запоминается и возвращается обратно через
    // restoreMs. Картинки и прочие mime-типы не сохраняются.
    void setPreserveClipboard(bool preserve);
    void setClipboardRestoreMs(int ms);

    // --- привязка к окну ---
    //
    // Без привязки текст уходит в то окно, которое в фокусе В МОМЕНТ ПРИХОДА
    // результата, а это через 0.5–2 с после произнесённой фразы. Стоит
    // переключиться в другой файл, чтобы сделать пометку, — и продиктованное
    // прилетит туда.
    //
    // С привязкой окно запоминается в начале записи, и вставка идёт в него,
    // даже если фокус уже в другом месте.
    //
    // КАК ИМЕННО ДОСТАВЛЯЕТСЯ ТЕКСТ (главное, что здесь было сломано).
    // `xdotool … --window WID` шлёт события через XSendEvent, и они помечены
    // флагом send_event=1. Qt и GTK такие события принимают, а вот браузеры,
    // LibreOffice, Java/Eclipse и часть терминалов — отбрасывают. xdotool при
    // этом возвращает 0, поэтому в логе всё выглядело успешным, а в редакторе
    // не появлялось ничего. Отсюда два режима (см. output/WindowTarget.h):
    //
    //   PinMode::Activate  (по умолчанию) — окно активируется, текст печатается
    //                  НАСТОЯЩИМИ событиями (XTest), затем фокус возвращается.
    //                  Работает в любом приложении ценой ~50–150 мс фокуса.
    //   PinMode::SendEvent — прежнее поведение: --window, фокус не трогаем.
    //                  Нужно, если вы принципиально печатаете руками в другом
    //                  окне во время диктовки И ваше приложение принимает
    //                  синтетику. Проверить: ./src/voice-assistant --type "раз"
    //                  --pin-active, переключившись в другое окно.
    void    setPinnedWindow(const QString& windowId);
    QString pinnedWindow() const { return m_pinnedWindow; }
    void    clearPinnedWindow() { m_pinnedWindow.clear(); }

    void    setPinMode(PinMode mode);
    PinMode pinMode() const { return m_pinMode; }
    void    setPinRestoreFocus(bool restore);   // возвращать фокус после активации
    void    setPinActivateDelayMs(int ms);      // пауза после активации, до печати
    // WM_CLASS окон самого помощника: в них вставлять нельзя (текст уйдёт в
    // меню трея и пропадёт вместе с ним). Заполняется из [output] own_window_class.
    void    setOwnWindowClasses(const QStringList& classes);

    // --- диагностика окон (статические, чтобы ими пользовался и контроллер) ---
    //
    // WID активного окна ("xdotool getactivewindow"). Пустая строка, если
    // xdotool не найден, X-сервера нет или окно не определилось.
    static QString activeWindowId();
    // Заголовок окна ("xdotool getwindowname"). Пусто, если окна нет: по этому
    // признаку отличаем закрытое окно от живого.
    static QString windowName(const QString& windowId);
    // WM_CLASS окна — по нему узнаём «это окно нашего же помощника».
    static QString windowClass(const QString& windowId);
    // PID владельца окна (0, если не определился).
    static qint64  windowPid(const QString& windowId);
    // Жива ли ещё привязка: окно существует и у него читается заголовок.
    static bool    windowExists(const QString& windowId);
    // Человекочитаемое описание окна для логов: "12345678 «document.txt — Kate»".
    static QString describeWindow(const QString& windowId);

    QString backendName() const override;   // для логов и SettingsDialog

    // Способ вставки словами — для строки инициализации в логе. В dry-run
    // backendName() равен «dry-run», и без этого описания в логе не видно,
    // чем именно будет печататься текст.
    QString methodDescription() const;

    // Последняя ошибка — текст для reportError() и для лога.
    QString lastError() const { return m_lastError; }

    // Какое решение принято для текущей привязки (для --pin-info и тестов).
    WindowTarget resolveTarget() const;

private:
    // Скоуп доставки: при Delivery::Activate активирует окно в конструкторе и
    // возвращает фокус в деструкторе. Один скоуп на всю вставку — иначе при
    // откате «буфер не сработал -> xdotool type» фокус мигал бы дважды.
    struct DeliveryScope {
        XdotoolInjector* self = nullptr;
        QString restoreTo;
        bool restore = false;
        ~DeliveryScope();
    };
    DeliveryScope enterTarget(const WindowTarget& t);

    bool typeViaXdotool(const WindowTarget& t, const QString& text);
    bool typeViaClipboard(const WindowTarget& t, const QString& text);
    bool sendKeyTo(const WindowTarget& t, const QString& key);
    bool runProcess(const QString& program,
                    const QStringList& args,
                    const QByteArray& stdinData = QByteArray());
    // xdotool с чтением stdout и коротким таймаутом — для диагностики окон.
    static QString runXdotool(const QStringList& args, int timeoutMs = 2000);
    // Аргументы "--window WID" для синтетической доставки (пустые при активации).
    QStringList windowArgs(const WindowTarget& t) const;
    void logDelivery(const QString& what, const WindowTarget& t, bool ok) const;
    // Привязка есть, а окно недоступно — сказать об этом вслух, иначе текст
    // молча уйдёт не туда (главный симптом, который мы чиним).
    void warnIfPinLost(const WindowTarget& t) const;

    Method  m_method        = Method::Auto;
    int     m_typingDelayMs = 12;
    bool    m_available     = false;
    bool    m_dryRun        = false;

    bool    m_preserveClipboard = true;
    int     m_clipboardRestoreMs = 1000;

    QString     m_pinnedWindow;    // WID окна-цели; пусто = вставлять в активное
    PinMode     m_pinMode = PinMode::Activate;
    bool        m_pinRestoreFocus = true;
    int         m_pinActivateDelayMs = 80;
    QStringList m_ownWindowClasses;

    QString     m_lastError;

    QString     m_xdotool;         // полный путь к xdotool
    QString     m_clipboardTool;   // xclip / xsel / wl-copy
    QStringList m_clipboardArgs;   // аргументы для записи в буфер
};
