// src/main.cpp
#include "core/ApplicationController.h"
#include "dbus/DBusInterface.h"
#include "ui/TrayIcon.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

namespace {
const char* kDBusService = "org.voiceassistant.App";
const char* kDBusPath    = "/org/voiceassistant/App";
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("voice-assistant");
    app.setApplicationVersion("0.0.1");

    ApplicationController controller;
    TrayIcon trayIcon(&controller);
    trayIcon.show();

    // D-Bus: адаптор соответствует src/dbus/org.voiceassistant.App.xml.
    // Отсутствие сессии (чистая TTY, systemd-юнит без bus) не должно ронять приложение.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        new DBusInterface(&controller);   // адаптор становится потомком контроллера

        if (!bus.registerObject(kDBusPath, &controller)) {
            qWarning().noquote() << QString("D-Bus: не зарегистрировал объект %1: %2")
                                        .arg(kDBusPath, bus.lastError().message());
        } else if (!bus.registerService(kDBusService)) {
            qWarning().noquote() << QString("D-Bus: не зарегистрировал сервис %1: %2 "
                                            "(возможно, помощник уже запущен)")
                                        .arg(kDBusService, bus.lastError().message());
        } else {
            qInfo().noquote() << QString("D-Bus: %1 на %2")
                                     .arg(kDBusService, kDBusPath);
        }
    } else {
        qWarning() << "D-Bus: сессионная шина недоступна, внешнее управление выключено";
    }

    return app.exec();
}
