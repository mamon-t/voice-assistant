#pragma once

#include <QObject>
#include <QByteArray>

class IAudioCapture : public QObject {
    Q_OBJECT

public:
    explicit IAudioCapture(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~IAudioCapture() = default;
    
    virtual bool initialize() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    
signals:
    void audioDataReady(const QByteArray& data, int sampleRate);
    void errorOccurred(const QString& message);
};