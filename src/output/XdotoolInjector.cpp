#include "output/XdotoolInjector.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {

// Буфер обмена доступен, только если создан QGuiApplication (в QApplication он есть).
// В консольных инструментах с QCoreApplication его нет — не падаем, а пропускаем.
QClipboard* clipboard()
{
    auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    return gui ? gui->clipboard() : nullptr;
}

}  // namespace

namespace {

int kProcessTimeoutMs = 5000;

}  // namespace

XdotoolInjector::XdotoolInjector() = default;
XdotoolInjector::~XdotoolInjector() = default;

bool XdotoolInjector::initialize()
{
    m_dryRun = qEnvironmentVariableIsSet("VOICE_ASSISTANT_DRYRUN");

    m_xdotool = QStandardPaths::findExecutable(QStringLiteral("xdotool"));

    // Инструмент буфера обмена: первый найденный
    struct Tool { const char* name; QStringList args; };
    const Tool tools[] = {
        { "xclip",   { QStringLiteral("-selection"), QStringLiteral("clipboard"), QStringLiteral("-in") } },
        { "xsel",    { QStringLiteral("--clipboard"), QStringLiteral("--input") } },
        { "wl-copy", {} },
    };
    m_clipboardTool.clear();
    m_clipboardArgs.clear();
    for (const Tool& t : tools) {
        const QString path = QStandardPaths::findExecutable(QString::fromLatin1(t.name));
        if (!path.isEmpty()) {
            m_clipboardTool = path;
            m_clipboardArgs = t.args;
            break;
        }
    }

    m_available = m_dryRun || !m_xdotool.isEmpty();

    if (!m_available) {
        qCritical().noquote()
            << "XdotoolInjector: xdotool не найден в PATH. Поставь его (sudo apt install xdotool) "
               "или используй ydotool для Wayland/TTY.";
        return false;
    }

    qInfo().noquote() << QString("XdotoolInjector: xdotool=%1, буфер=%2, режим=%3%4")
                             .arg(m_xdotool,
                                  m_clipboardTool.isEmpty() ? QStringLiteral("нет") : m_clipboardTool,
                                  backendName(),
                                  m_dryRun ? QStringLiteral(" (DRY RUN)") : QString());
    return true;
}

void XdotoolInjector::setMethod(Method method)
{
    m_method = method;
}

void XdotoolInjector::setTypingDelayMs(int ms)
{
    m_typingDelayMs = (ms >= 0) ? ms : 0;
}

void XdotoolInjector::setPreserveClipboard(bool preserve)
{
    m_preserveClipboard = preserve;
}

void XdotoolInjector::setClipboardRestoreMs(int ms)
{
    m_clipboardRestoreMs = (ms >= 0) ? ms : 1000;
}

QString XdotoolInjector::backendName() const
{
    if (m_dryRun) {
        return QStringLiteral("dry-run");
    }
    const bool haveClipboard = !m_clipboardTool.isEmpty();
    switch (m_method) {
    case Method::XdotoolType: return QStringLiteral("xdotool type");
    case Method::Clipboard:   return haveClipboard ? QStringLiteral("clipboard+ctrl+v")
                                                   : QStringLiteral("xdotool type (буфер недоступен)");
    case Method::Auto:        return haveClipboard ? QStringLiteral("clipboard+ctrl+v")
                                                   : QStringLiteral("xdotool type");
    }
    return QStringLiteral("unknown");
}

bool XdotoolInjector::isAvailable() const
{
    return m_available;
}

bool XdotoolInjector::typeText(const QString& text)
{
    if (!m_available) {
        qWarning() << "XdotoolInjector::typeText: инжектор не инициализирован";
        return false;
    }
    if (text.isEmpty()) {
        return true;
    }

    if (m_dryRun) {
        qInfo().noquote() << QString("[dry-run] typeText: %1").arg(text);
        return true;
    }

    const bool preferClipboard =
        (m_method == Method::Clipboard)
        || (m_method == Method::Auto && !m_clipboardTool.isEmpty());

    if (preferClipboard && !m_clipboardTool.isEmpty()) {
        if (typeViaClipboard(text)) {
            return true;
        }
        qWarning() << "XdotoolInjector: буфер обмена не сработал, пробую xdotool type";
    }
    return typeViaXdotool(text);
}

bool XdotoolInjector::sendKey(const QString& key)
{
    if (!m_available) {
        qWarning() << "XdotoolInjector::sendKey: инжектор не инициализирован";
        return false;
    }
    if (key.isEmpty()) {
        return true;
    }
    if (m_dryRun) {
        qInfo().noquote() << QString("[dry-run] sendKey: %1").arg(key);
        return true;
    }
    if (m_xdotool.isEmpty()) {
        qWarning() << "XdotoolInjector::sendKey: xdotool не найден";
        return false;
    }

    return runProcess(m_xdotool,
                      { QStringLiteral("key"), QStringLiteral("--clearmodifiers"), key });
}

bool XdotoolInjector::typeViaXdotool(const QString& text)
{
    if (m_xdotool.isEmpty()) {
        return false;
    }
    // --file - читает текст из stdin: так мы не упираемся в лимит длины
    // аргумента командной строки и не воюем с экранированием кавычек.
    return runProcess(m_xdotool,
                      { QStringLiteral("type"),
                        QStringLiteral("--clearmodifiers"),
                        QStringLiteral("--delay"), QString::number(m_typingDelayMs),
                        QStringLiteral("--file"), QStringLiteral("-") },
                      text.toUtf8());
}

bool XdotoolInjector::typeViaClipboard(const QString& text)
{
    if (m_clipboardTool.isEmpty()) {
        return false;
    }

    // Запоминаем, что лежало в буфере, чтобы вернуть после вставки
    QString saved;
    bool haveSaved = false;
    if (m_preserveClipboard) {
        if (QClipboard* cb = clipboard()) {
            saved = cb->text();
            haveSaved = true;
        }
    }

    if (!runProcess(m_clipboardTool, m_clipboardArgs, text.toUtf8())) {
        return false;
    }
    if (!runProcess(m_xdotool,
                    { QStringLiteral("key"), QStringLiteral("--clearmodifiers"),
                      QStringLiteral("ctrl+v") })) {
        return false;
    }

    if (haveSaved) {
        // Возвращаем не сразу: целевое приложение должно успеть забрать текст
        // из буфера по Ctrl+V. Задержка настраивается [output] clipboard_restore_ms.
        const int delay = m_clipboardRestoreMs;
        QTimer::singleShot(delay, [saved]() {
            if (QClipboard* cb = clipboard()) {
                cb->setText(saved);
            }
        });
    }
    return true;
}

bool XdotoolInjector::runProcess(const QString& program,
                                 const QStringList& args,
                                 const QByteArray& stdinData)
{
    if (program.isEmpty()) {
        return false;
    }

    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.start();

    if (!proc.waitForStarted(kProcessTimeoutMs)) {
        qWarning().noquote() << QString("XdotoolInjector: не запустил %1 %2")
                                    .arg(program, args.join(QLatin1Char(' ')));
        return false;
    }

    if (!stdinData.isEmpty()) {
        proc.write(stdinData);
    }
    proc.closeWriteChannel();

    if (!proc.waitForFinished(kProcessTimeoutMs)) {
        qWarning().noquote() << QString("XdotoolInjector: %1 не завершился за %2 мс")
                                    .arg(program).arg(kProcessTimeoutMs);
        proc.kill();
        proc.waitForFinished(500);
        return false;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        qWarning().noquote()
            << QString("XdotoolInjector: %1 вернул %2: %3")
                   .arg(program)
                   .arg(proc.exitCode())
                   .arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}
