#pragma once

#include "output/ITextInjector.h"

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
    // С привязкой окно запоминается в начале записи, и вставка идёт в него
    // (xdotool ... --window WID), даже если фокус уже в другом месте:
    // можно диктовать и параллельно печатать руками.
    //
    // ЧЕСТНО ПРО ОГРАНИЧЕНИЕ: `xdotool --window` доставляет события через
    // XSendEvent, и часть приложений такие события игнорирует (в первую
    // очередь это касается `key`, то есть способа «буфер обмена + Ctrl+V»).
    // Поэтому привязка выключена по умолчанию, а при первом неподтверждённом
    // вводе пишется предупреждение в лог. Проверить на своём редакторе:
    //   [output] pin_window=true  ->  надиктовать фразу, переключив окно.
    void    setPinnedWindow(const QString& windowId);
    QString pinnedWindow() const { return m_pinnedWindow; }
    void    clearPinnedWindow() { m_pinnedWindow.clear(); }

    // WID активного окна ("xdotool getactivewindow"). Пустая строка, если
    // xdotool не найден, X-сервера нет или окно не определилось.
    static QString activeWindowId();

    QString backendName() const override;   // для логов и SettingsDialog

private:
    bool typeViaXdotool(const QString& text);
    bool typeViaClipboard(const QString& text);
    bool runProcess(const QString& program,
                    const QStringList& args,
                    const QByteArray& stdinData = QByteArray());

    Method  m_method        = Method::Auto;
    int     m_typingDelayMs = 12;
    bool    m_available     = false;
    bool    m_dryRun        = false;

    bool    m_preserveClipboard = true;
    int     m_clipboardRestoreMs = 1000;

    QString     m_pinnedWindow;    // WID окна-цели; пусто = вставлять в активное

    QString     m_xdotool;         // полный путь к xdotool
    QString     m_clipboardTool;   // xclip / xsel / wl-copy
    QStringList m_clipboardArgs;   // аргументы для записи в буфер
};
