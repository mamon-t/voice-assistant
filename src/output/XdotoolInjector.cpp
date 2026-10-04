#include "output/XdotoolInjector.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

namespace {

// Буфер обмена доступен, только если создан QGuiApplication (в QApplication он есть).
// В консольных инструментах с QCoreApplication его нет — не падаем, а пропускаем.
QClipboard* clipboard()
{
    auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    return gui ? gui->clipboard() : nullptr;
}

int kProcessTimeoutMs = 5000;

// Пауза после windowactivate, если [output] pin_activate_ms не задан.
// 80 мс хватает openbox/KDE/GNOME, чтобы передать фокус приложению; на
// медленной машине можно поднять до 200–300.
int kDefaultActivateDelayMs = 80;

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

    // Режим привязки пишем сюда, а не только в backendName(): в dry-run
    // backendName() равен «dry-run», и без этой строки в логе не видно,
    // каким способом пойдёт текст.
    qInfo().noquote() << QString("XdotoolInjector: xdotool=%1, буфер=%2, режим=%3, "
                                 "способ=%4, привязка к окну=%5%6")
                             .arg(m_xdotool,
                                  m_clipboardTool.isEmpty() ? QStringLiteral("нет") : m_clipboardTool,
                                  backendName(),
                                  methodDescription(),
                                  m_pinnedWindow.isEmpty() ? QStringLiteral("нет")
                                                           : m_pinnedWindow,
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

void XdotoolInjector::setPinnedWindow(const QString& windowId)
{
    const QString wid = windowId.trimmed();
    if (wid == m_pinnedWindow) {
        return;
    }
    m_pinnedWindow = wid;
    if (wid.isEmpty()) {
        qDebug().noquote() << QStringLiteral("XdotoolInjector: привязка к окну снята, "
                                            "вставка идёт в активное окно");
        return;
    }

    // Сразу показываем, ЧТО это за окно и КАК будет доставлен текст: баг
    // «лог виден, а в редакторе ничего» лечится в первую очередь тем, что
    // цель перестаёт быть неизвестной.
    qInfo().noquote()
        << QStringLiteral("XdotoolInjector: вставка привязана к окну %1, способ — %2")
               .arg(describeWindow(wid),
                    m_pinMode == PinMode::SendEvent
                        ? QStringLiteral("синтетические события (--window), фокус не трогаем")
                        : QStringLiteral("активация окна + настоящие события, фокус вернётся обратно"));
}

void XdotoolInjector::setPinMode(PinMode mode)
{
    m_pinMode = mode;
}

void XdotoolInjector::setPinRestoreFocus(bool restore)
{
    m_pinRestoreFocus = restore;
}

void XdotoolInjector::setPinActivateDelayMs(int ms)
{
    m_pinActivateDelayMs = (ms >= 0) ? ms : kDefaultActivateDelayMs;
}

void XdotoolInjector::setOwnWindowClasses(const QStringList& classes)
{
    m_ownWindowClasses = classes;
}

// ---------------------------------------------------------------------------
// Диагностика окон
// ---------------------------------------------------------------------------

QString XdotoolInjector::runXdotool(const QStringList& args, int timeoutMs)
{
    const QString xdotool = QStandardPaths::findExecutable(QStringLiteral("xdotool"));
    if (xdotool.isEmpty()) {
        return QString();
    }
    QProcess proc;
    proc.setProgram(xdotool);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForStarted(timeoutMs) || !proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(200);
        return QString();
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        return QString();
    }
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

QString XdotoolInjector::activeWindowId()
{
    return runXdotool({QStringLiteral("getactivewindow")});
}

QString XdotoolInjector::windowName(const QString& windowId)
{
    const QString wid = windowId.trimmed();
    if (wid.isEmpty()) {
        return QString();
    }
    return runXdotool({QStringLiteral("getwindowname"), wid});
}

QString XdotoolInjector::windowClass(const QString& windowId)
{
    const QString wid = windowId.trimmed();
    if (wid.isEmpty()) {
        return QString();
    }
    // getwindowclassname есть не во всех сборках xdotool — тогда берём WM_CLASS
    // через xprop. Оба варианта возвращают пусто, если окна не существует.
    QString cls = runXdotool({QStringLiteral("getwindowclassname"), wid});
    if (!cls.isEmpty()) {
        return cls;
    }
    const QString xprop = QStandardPaths::findExecutable(QStringLiteral("xprop"));
    if (xprop.isEmpty()) {
        return QString();
    }
    QProcess proc;
    proc.setProgram(xprop);
    proc.setArguments({QStringLiteral("-id"), wid, QStringLiteral("WM_CLASS")});
    proc.start();
    if (!proc.waitForStarted(1000) || !proc.waitForFinished(1000)) {
        proc.kill();
        proc.waitForFinished(200);
        return QString();
    }
    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    // WM_CLASS(STRING) = "res_name", "res_class" — нужен ВТОРОЙ поле:
    // getwindowclassname возвращает именно res_class, запасной путь обязан
    // вести себя так же. (У Qt res_name — имя бинарника, res_class —
    // applicationName; сравниваем мы с [output] own_window_class.)
    const int q1 = out.indexOf(QLatin1Char('"'));
    const int q2 = out.indexOf(QLatin1Char('"'), q1 + 1);
    const int q3 = out.indexOf(QLatin1Char('"'), q2 + 1);
    const int q4 = out.indexOf(QLatin1Char('"'), q3 + 1);
    if (q3 >= 0 && q4 > q3) {
        return out.mid(q3 + 1, q4 - q3 - 1);
    }
    if (q1 >= 0 && q2 > q1) {
        return out.mid(q1 + 1, q2 - q1 - 1);
    }
    return QString();
}

qint64 XdotoolInjector::windowPid(const QString& windowId)
{
    const QString wid = windowId.trimmed();
    if (wid.isEmpty()) {
        return 0;
    }
    bool ok = false;
    const qint64 pid = runXdotool({QStringLiteral("getwindowpid"), wid}).toLongLong(&ok);
    return ok ? pid : 0;
}

bool XdotoolInjector::windowExists(const QString& windowId)
{
    const QString wid = windowId.trimmed();
    if (wid.isEmpty()) {
        return false;
    }
    // getwindowname — самый дешёвый способ проверить жизнь окна: на закрытом
    // xdotool падает в BadWindow и возвращает ненулевой код. Заголовок при этом
    // бывает пустым у окон без имени, поэтому на пустой строке дополнительно
    // спрашиваем PID.
    if (!runXdotool({QStringLiteral("getwindowname"), wid}).isEmpty()) {
        return true;
    }
    return windowPid(wid) > 0;
}

QString XdotoolInjector::describeWindow(const QString& windowId)
{
    const QString wid = windowId.trimmed();
    if (wid.isEmpty()) {
        return QStringLiteral("(окно не определено)");
    }
    const QString name = windowName(wid);
    if (name.isEmpty()) {
        return QStringLiteral("%1 (без заголовка)").arg(wid);
    }
    return QStringLiteral("%1 «%2»").arg(wid, name);
}

WindowTarget XdotoolInjector::resolveTarget() const
{
    if (m_pinnedWindow.isEmpty()) {
        WindowTarget t;
        t.delivery = WindowTarget::Delivery::ActiveWindow;
        return t;
    }

    // Окна самого помощника определяем прежде всего по WM_CLASS: меню трея —
    // это отдельное окно того же процесса, и в него вставлять нельзя. PID —
    // страховка на случай, когда класс не прочитался.
    QStringList own = m_ownWindowClasses;
    if (own.isEmpty()) {
        own << QStringLiteral("voice-assistant");
    }
    QStringList ownWids;
    const QString cls = windowClass(m_pinnedWindow);
    if (!cls.isEmpty() && own.contains(cls)) {
        ownWids << m_pinnedWindow;
    } else if (windowPid(m_pinnedWindow) == QCoreApplication::applicationPid()) {
        // Страховка второго рубежа: WM_CLASS мог не прочитаться (в релизном
        // xdotool нет getwindowclassname, а xprop не установлен), но PID
        // доступен почти всегда. Окно собственного процесса легитимной целью
        // не бывает — это либо меню лотка, либо диалог помощника. --pin-info
        // проверяет PID по тем же причинам.
        ownWids << m_pinnedWindow;
    }

    return decideWindowTarget(m_pinnedWindow,
                              activeWindowId(),
                              windowExists(m_pinnedWindow),
                              m_pinMode,
                              m_pinRestoreFocus,
                              ownWids);
}

QString XdotoolInjector::backendName() const
{
    if (m_dryRun) {
        return QStringLiteral("dry-run");
    }
    const bool haveClipboard = !m_clipboardTool.isEmpty();
    QString name;
    switch (m_method) {
    case Method::XdotoolType: name = QStringLiteral("xdotool type"); break;
    case Method::Clipboard:   name = haveClipboard
                                     ? QStringLiteral("clipboard+ctrl+v")
                                     : QStringLiteral("xdotool type (буфер недоступен)"); break;
    case Method::Auto:        name = haveClipboard ? QStringLiteral("clipboard+ctrl+v")
                                                   : QStringLiteral("xdotool type"); break;
    }
    if (!name.isEmpty() && !m_pinnedWindow.isEmpty()) {
        name += QStringLiteral(" -> окно %1 (%2)")
                    .arg(m_pinnedWindow,
                         m_pinMode == PinMode::SendEvent ? QStringLiteral("sendevent")
                                                         : QStringLiteral("activate"));
    }
    return name.isEmpty() ? QStringLiteral("unknown") : name;
}

QString XdotoolInjector::methodDescription() const
{
    switch (m_method) {
    case Method::XdotoolType: return QStringLiteral("xdotool type");
    case Method::Clipboard:   return QStringLiteral("буфер обмена + Ctrl+V");
    case Method::Auto:        return m_clipboardTool.isEmpty()
                                     ? QStringLiteral("auto -> xdotool type")
                                     : QStringLiteral("auto -> буфер обмена + Ctrl+V");
    }
    return QStringLiteral("unknown");
}

bool XdotoolInjector::isAvailable() const
{
    return m_available;
}

// ---------------------------------------------------------------------------
// Доставка
// ---------------------------------------------------------------------------

XdotoolInjector::DeliveryScope::~DeliveryScope()
{
    if (!restore || restoreTo.isEmpty() || !self) {
        return;
    }
    // Даём целевому приложению обработать последние нажатия: возврат фокуса
    // соревнуется с их обработкой, и Qt отбрасывает клавиши, пришедшие уже
    // НЕактивному окну — теряется последний символ (поймано e2e-стендом:
    // «тест три чере» вместо «тест три четыре» при возврате фокуса вплотную
    // к последнему нажатию).
    QThread::msleep(60);
    // Возвращаем фокус туда, где он был до вставки. Не aktivatе --sync
    // намеренно: ждать подтверждения не нужно, а зависнуть на закрытом окне —
    // очень даже. Ошибку логируем, но не считаем провалом вставки.
    const bool ok = self->runProcess(self->m_xdotool,
                                     {QStringLiteral("windowactivate"), restoreTo});
    if (!ok) {
        qWarning().noquote()
            << QStringLiteral("XdotoolInjector: не вернул фокус окну %1").arg(restoreTo);
    }
}

XdotoolInjector::DeliveryScope XdotoolInjector::enterTarget(WindowTarget& t)
{
    DeliveryScope scope;
    scope.self = this;
    if (!t.needsActivation() || m_dryRun || m_xdotool.isEmpty()) {
        return scope;
    }

    // Понижение до синтетики: настоящие события без --window уходят в окно,
    // которое В ФОКУСЕ. Если активация не удалась, в фокусе не наше окно —
    // текст ушёл бы в случайное место, а xdotool всё равно вернул бы 0.
    // Синтетика в правильное окно лучше: Qt/GTK её принимают, а браузеры —
    // нет, но тогда это видно в логе ([синтетика (--window)]).
    const auto downgradeToSendEvent = [this, &t](const QString& why) {
        qWarning().noquote()
            << QStringLiteral("XdotoolInjector: %1 — переключаюсь на доставку "
                              "через --window (синтетика): часть приложений её "
                              "игнорирует, смотрите строку вставки в логе").arg(why);
        t.delivery = WindowTarget::Delivery::SendEvent;
        t.restoreTo.clear();
    };

    // --sync ждёт, пока WM действительно отдаст фокус. На закрытом окне он
    // может ждать вечно, поэтому сначала проверяем, что окно живое, и держим
    // короткий таймаут процесса (runProcess).
    if (!runProcess(m_xdotool,
                    {QStringLiteral("windowactivate"), QStringLiteral("--sync"), t.window})) {
        // План Б: XSetInputFocus напрямую. Не требует поддержки EWMH
        // оконным менеджером и не висит.
        qWarning().noquote()
            << QStringLiteral("XdotoolInjector: windowactivate --sync не сработал для %1, "
                              "пробую windowfocus").arg(t.window);
        if (!runProcess(m_xdotool, {QStringLiteral("windowfocus"), t.window})) {
            m_lastError = QStringLiteral("не удалось активировать окно %1").arg(t.window);
            downgradeToSendEvent(QStringLiteral("не удалось активировать окно"));
            return scope;
        }
    }
    if (m_pinActivateDelayMs > 0) {
        QThread::msleep(static_cast<unsigned long>(m_pinActivateDelayMs));
    }

    // Доверяй, но проверяй: WM может отрапортовать об активации, не переместив
    // фокус (наблюдено в openbox на свежепостроенных окнах, пока не выставлен
    // _NET_WM_DESKTOP). Одна повторная попытка, затем понижение до синтетики.
    // Пустой ответ getactivewindow провалом не считаем: значит, WM не
    // рапортует активное окно вовсе, и проверять нечем.
    const QString nowActive = activeWindowId();
    if (!nowActive.isEmpty() && nowActive != t.window) {
        qDebug().noquote()
            << QStringLiteral("XdotoolInjector: фокус не переехал в окно %1 — "
                              "повторяю активацию").arg(t.window);
        runProcess(m_xdotool,
                   {QStringLiteral("windowactivate"), QStringLiteral("--sync"), t.window});
        if (m_pinActivateDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(m_pinActivateDelayMs));
        }
        const QString retry = activeWindowId();
        if (!retry.isEmpty() && retry != t.window) {
            downgradeToSendEvent(QStringLiteral("окно не стало активным после "
                                                "двух попыток активации"));
            return scope;
        }
    }

    scope.restoreTo = t.restoreTo;
    scope.restore   = !t.restoreTo.isEmpty();
    return scope;
}

QStringList XdotoolInjector::windowArgs(const WindowTarget& t) const
{
    // При активации окно уже в фокусе: --window не нужен и даже вреден —
    // с ним xdotool может уйти в XSendEvent вместо настоящих событий.
    if (t.usesWindowFlag() && !t.window.isEmpty()) {
        return {QStringLiteral("--window"), t.window};
    }
    return {};
}

void XdotoolInjector::warnIfPinLost(const WindowTarget& t) const
{
    // Привязка была, но цель отброшена: окно закрыли или это окно самого
    // помощника (меню трея, настройки). Молчать здесь нельзя: именно из
    // такого молчания и рождается «лог виден, а в редакторе ничего».
    // ВНИМАНИЕ: «окно уже в фокусе» (pinLost == false при ActiveWindow) —
    // нормальная доставка настоящими событиями, предупреждать не о чем.
    if (m_pinnedWindow.isEmpty() || !t.pinLost) {
        return;
    }
    qWarning().noquote()
        << QStringLiteral("XdotoolInjector: привязанное окно %1 недоступно "
                          "(закрыто или принадлежит самому помощнику), "
                          "текст пойдёт в активное окно")
               .arg(describeWindow(m_pinnedWindow));
}

void XdotoolInjector::logDelivery(const QString& what, const WindowTarget& t, bool ok) const
{
    const QString where = (t.delivery == WindowTarget::Delivery::ActiveWindow)
        ? QStringLiteral("активное окно %1").arg(activeWindowId())
        : describeWindow(t.window);

    if (ok) {
        qInfo().noquote()
            << QStringLiteral("XdotoolInjector: %1 -> %2 [%3]")
                   .arg(what, where, deliveryToString(t.delivery));
        return;
    }
    qWarning().noquote()
        << QStringLiteral("XdotoolInjector: НЕ ВСТАВИЛ %1 -> %2 [%3]: %4")
               .arg(what, where, deliveryToString(t.delivery),
                    m_lastError.isEmpty() ? QStringLiteral("xdotool вернул ошибку") : m_lastError);
}

bool XdotoolInjector::typeText(const QString& text)
{
    m_lastError.clear();
    if (!m_available) {
        m_lastError = QStringLiteral("инжектор не инициализирован");
        qWarning() << "XdotoolInjector::typeText:" << m_lastError;
        return false;
    }
    if (text.isEmpty()) {
        return true;
    }

    if (m_dryRun) {
        qInfo().noquote() << QString("[dry-run] typeText: %1").arg(text);
        return true;
    }

    // Решение принимается ОДИН раз на всю вставку: иначе при откате
    // «буфер не сработал -> xdotool type» окно активировалось бы дважды
    // и фокус мигал бы на каждом сегменте. Не const: enterTarget может
    // понизить доставку до SendEvent, если активация не удалась.
    WindowTarget t = resolveTarget();
    warnIfPinLost(t);
    DeliveryScope scope = enterTarget(t);

    const bool preferClipboard =
        (m_method == Method::Clipboard)
        || (m_method == Method::Auto && !m_clipboardTool.isEmpty());

    if (preferClipboard && !m_clipboardTool.isEmpty()) {
        if (typeViaClipboard(t, text)) {
            logDelivery(QStringLiteral("текст (%1 симв.)").arg(text.size()), t, true);
            return true;
        }
        qWarning().noquote()
            << QStringLiteral("XdotoolInjector: буфер обмена не сработал (%1), "
                              "пробую xdotool type").arg(m_lastError);
    }
    const bool ok = typeViaXdotool(t, text);
    logDelivery(QStringLiteral("текст (%1 симв.)").arg(text.size()), t, ok);
    return ok;
}

bool XdotoolInjector::sendKey(const QString& key)
{
    m_lastError.clear();
    if (!m_available) {
        m_lastError = QStringLiteral("инжектор не инициализирован");
        qWarning() << "XdotoolInjector::sendKey:" << m_lastError;
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
        m_lastError = QStringLiteral("xdotool не найден");
        qWarning() << "XdotoolInjector::sendKey:" << m_lastError;
        return false;
    }

    WindowTarget t = resolveTarget();
    warnIfPinLost(t);
    DeliveryScope scope = enterTarget(t);
    const bool ok = sendKeyTo(t, key);
    logDelivery(QStringLiteral("клавиша %1").arg(key), t, ok);
    return ok;
}

bool XdotoolInjector::sendKeyTo(const WindowTarget& t, const QString& key)
{
    QStringList args{ QStringLiteral("key") };
    args << windowArgs(t) << QStringLiteral("--clearmodifiers") << key;
    if (!runProcess(m_xdotool, args)) {
        m_lastError = QStringLiteral("xdotool key %1 вернул ошибку").arg(key);
        return false;
    }
    return true;
}

bool XdotoolInjector::typeViaXdotool(const WindowTarget& t, const QString& text)
{
    if (m_xdotool.isEmpty()) {
        m_lastError = QStringLiteral("xdotool не найден");
        return false;
    }

    // Честное предупреждение про посимвольную печать (одноразовое). xdotool для
    // символов вне текущей раскладки временно переназначает keycode; приложение
    // получает MappingNotify и нажатия одновременно, и часть нажатий
    // декодируется ещё СТАРОЙ раскладкой — символы случайно теряются или
    // подменяются (проверено на Qt5/Xvfb: с --delay 5 теряется, с --delay 60
    // нет; при sendevent в расфокусированное окно — чаще). Это свойство
    // xdotool, а не наше; буфер обмена от него не зависит.
    Q_UNUSED(t);
    if (!m_warnedSyntheticTyping) {
        m_warnedSyntheticTyping = true;
        qWarning().noquote()
            << QStringLiteral("XdotoolInjector: посимвольная печать (method=xdotool): "
                              "для символов вне текущей раскладки xdotool временно "
                              "переназначает keycode, и из-за гонки MappingNotify "
                              "теряются отдельные символы (чаще при sendevent в "
                              "расфокусированное окно). Если текст приходит с "
                              "дырками — используйте [output] method=auto (буфер "
                              "обмена) или добавьте русскую раскладку в keymap");
    }

    // --file - читает текст из stdin: так мы не упираемся в лимит длины
    // аргумента командной строки и не воюем с экранированием кавычек.
    QStringList args{ QStringLiteral("type") };
    args << windowArgs(t)
         << QStringLiteral("--clearmodifiers")
         << QStringLiteral("--delay") << QString::number(m_typingDelayMs)
         << QStringLiteral("--file") << QStringLiteral("-");
    if (!runProcess(m_xdotool, args, text.toUtf8())) {
        m_lastError = QStringLiteral("xdotool type вернул ошибку");
        return false;
    }
    return true;
}

bool XdotoolInjector::typeViaClipboard(const WindowTarget& t, const QString& text)
{
    if (m_clipboardTool.isEmpty()) {
        m_lastError = QStringLiteral("нет инструмента буфера обмена (xclip/xsel/wl-copy)");
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
        m_lastError = QStringLiteral("%1 не принял текст").arg(m_clipboardTool);
        return false;
    }
    if (!sendKeyTo(t, QStringLiteral("ctrl+v"))) {
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
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        qWarning().noquote()
            << QString("XdotoolInjector: %1 вернул %2: %3")
                   .arg(program)
                   .arg(proc.exitCode())
                   .arg(err);
        if (!err.isEmpty() && m_lastError.isEmpty()) {
            m_lastError = err;
        }
        return false;
    }
    return true;
}
