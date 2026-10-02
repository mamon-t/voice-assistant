#pragma once

#include "audio/IAudioCapture.h"
#include <QAudioInput>
#include <QIODevice>

class QtAudioCapture : public IAudioCapture {
    Q_OBJECT

public:
    explicit QtAudioCapture(QObject* parent = nullptr);
    ~QtAudioCapture() override;

    bool initialize() override;
    void start() override;
    void stop() override;
    bool isRunning() const override;

private slots:
    void onAudioDataReady();
    void onStateChanged(QAudio::State state);

private:
    QAudioInput* m_audioInput = nullptr;
    QIODevice* m_device = nullptr;
    bool m_running = false;
    int m_sampleRate = 16000;
};