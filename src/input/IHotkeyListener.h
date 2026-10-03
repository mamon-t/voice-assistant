#pragma once

#include <QObject>
#include <QString>

// Слушатель глобального хоткея.
//
// Глобального — потому что QShortcut работает только когда окно приложения
// в фокусе, а диктовка нужна в любом приложении. Реализации:
//   EvdevHotkeyListener — читает /dev/input/event* напрямую (X11, Wayland, TTY),
//                         требует прав на /dev/input (группа `input`).
//
// Альтернатива без кода и без прав: повесить клавишу в настройках WM/DE на
//   dbus-send --session --type=method_call --dest=org.voiceassistant.App \
//     /org/voiceassistant/App org.voiceassistant.App.startRecording
class IHotkeyListener : public QObject {
    Q_OBJECT

public:
    explicit IHotkeyListener(QObject* parent = nullptr) : QObject(parent) {}
    ~IHotkeyListener() override = default;

    virtual bool start(QString* error = nullptr) = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;
    virtual QString backendName() const = 0;

signals:
    void pressed();
    void released();
    void errorOccurred(const QString& message);
};
