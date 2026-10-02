#pragma once

#include <QObject>
#include <QByteArray>
#include <memory>
#include "core/Mode.h"

class IAudioCapture;
class Agc;

class ApplicationController : public QObject {
    Q_OBJECT

public:
    explicit ApplicationController(QObject* parent = nullptr);
    ~ApplicationController();

    Mode mode() const;
    void startRecording();
    void stopRecording();
    void setMode(Mode mode);

signals:
    void modeChanged(Mode mode);
    void errorOccurred(const QString& message);

private slots:
    void onAudioDataReady(const QByteArray& data, int sampleRate);
    void onAudioError(const QString& message);

private:
    Mode m_mode = Mode::Off;
    std::unique_ptr<IAudioCapture> m_audioCapture;
    std::unique_ptr<Agc> m_agc;
    
    QByteArray m_audioBuffer;
    static constexpr int TARGET_CHUNK_SIZE = 1600; // 50 мс при 16 кГц
};