// src/main.cpp
#include "core/ApplicationController.h"
#include "ui/TrayIcon.h"
#include <QApplication>
#include <QDebug>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("voice-assistant");
    app.setApplicationVersion("0.0.1");
    
    qDebug() << "Voice Assistant skeleton started";
    qDebug() << "Qt version:" << qVersion();
    
    ApplicationController controller;
    TrayIcon trayIcon(&controller);
    trayIcon.show();
    
    return app.exec();
}