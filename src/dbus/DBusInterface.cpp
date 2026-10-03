#include "dbus/DBusInterface.h"
#include "core/ApplicationController.h"
#include "core/Mode.h"

DBusInterface::DBusInterface(ApplicationController* controller)
    : QDBusAbstractAdaptor(controller)
    , m_controller(controller)
{
    connect(m_controller, &ApplicationController::modeChanged,
            this, [this](Mode mode) {
        emit modeChanged(modeToString(mode));
    });

    // Раньше эти два сигнала адаптора никто не отправлял — внешний подписчик
    // (например, `dbus-monitor` или свой виджет) их просто не видел.
    connect(m_controller, &ApplicationController::textRecognized,
            this, [this](const QString& text) {
        emit textRecognized(text);
    });

    connect(m_controller, &ApplicationController::errorOccurred,
            this, [this](const QString& message) {
        emit errorOccurred(message);
    });
}

void DBusInterface::startRecording() {
    m_controller->startRecording();
}

void DBusInterface::stopRecording() {
    m_controller->stopRecording();
}

void DBusInterface::setMode(const QString& mode) {
    m_controller->setMode(stringToMode(mode));
}

QString DBusInterface::getMode() const {
    return modeToString(m_controller->mode());
}

void DBusInterface::reloadHotwords() {
    m_controller->reloadHotwords();
}

QString DBusInterface::startMicCheck() {
    QString path;
    if (!m_controller->startMicCheck(&path)) {
        return QString();
    }
    return path;
}

void DBusInterface::stopMicCheck() {
    m_controller->stopMicCheck();
}

bool DBusInterface::isMicChecking() const {
    return m_controller->isMicChecking();
}

QString DBusInterface::hotkeyStatus() const {
    return m_controller->hotkeyDescription();
}
