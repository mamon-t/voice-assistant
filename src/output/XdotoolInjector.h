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

    QString     m_xdotool;         // полный путь к xdotool
    QString     m_clipboardTool;   // xclip / xsel / wl-copy
    QStringList m_clipboardArgs;   // аргументы для записи в буфер
};
