#pragma once

#include <QDBusAbstractAdaptor>
#include <QString>

class ApplicationController;

class DBusInterface : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.voiceassistant.App")

public:
    explicit DBusInterface(ApplicationController* controller);

public slots:
    void startRecording();
    void stopRecording();
    void setMode(const QString& mode);
    QString getMode() const;
    void reloadHotwords();
    QString startMicCheck();      // путь к файлу записи
    void stopMicCheck();
    bool isMicChecking() const;
    QString hotkeyStatus() const;

signals:
    void modeChanged(const QString& mode);
    void textRecognized(const QString& text);
    void errorOccurred(const QString& message);

private:
    ApplicationController* m_controller;
};