#include "ApplicationController.h"
#include "audio/QtAudioCapture.h"
#include "audio/Agc.h"
#include <QDebug>
#include <cmath>
#include <cstdint>

ApplicationController::ApplicationController(QObject* parent)
    : QObject(parent)
{
    // 1. Создаём аудиоподсистему
    m_audioCapture = std::make_unique<QtAudioCapture>();
    
    if (!m_audioCapture->initialize()) {
        qCritical() << "Failed to initialize audio capture";
        emit errorOccurred("Failed to initialize audio capture");
        return;
    }
    
    // 2. Создаём и настраиваем AGC
    m_agc = std::make_unique<Agc>();
    m_agc->setTargetDb(-15.0f);
    m_agc->setAttackTime(0.05f);           // 50 мс
    m_agc->setReleaseTime(0.2f);           // 200 мс
    m_agc->setMaxGain(10.0f);              // Максимум 10x
    m_agc->setMinGain(0.01f);              // Минимум 0.01x
    m_agc->setNoiseGateThresholdDb(-35.0f); // Порог тишины -35 dB
    
    // 3. Подключаем сигналы
    QtAudioCapture* audioCapture = qobject_cast<QtAudioCapture*>(m_audioCapture.get());
    if (!audioCapture) {
        qCritical() << "Failed to cast to QtAudioCapture";
        emit errorOccurred("Audio capture type mismatch");
        return;
    }
    
    connect(audioCapture, &IAudioCapture::audioDataReady,
            this, &ApplicationController::onAudioDataReady);
    
    connect(audioCapture, &IAudioCapture::errorOccurred,
            this, &ApplicationController::onAudioError);
}

ApplicationController::~ApplicationController() {
    stopRecording();
}

Mode ApplicationController::mode() const {
    return m_mode;
}

void ApplicationController::startRecording() {
    if (m_mode == Mode::Off) {
        m_agc->reset();
        m_audioBuffer.clear();
        m_audioCapture->start();
        m_mode = Mode::Dictation;
        qDebug() << "Recording started";
        emit modeChanged(m_mode);
    }
}

void ApplicationController::stopRecording() {
    if (m_mode != Mode::Off) {
        m_audioCapture->stop();
        m_audioBuffer.clear();
        m_mode = Mode::Off;
        qDebug() << "Recording stopped";
        emit modeChanged(m_mode);
    }
}

void ApplicationController::setMode(Mode mode) {
    if (m_mode != mode) {
        if (m_mode == Mode::Off && mode != Mode::Off) {
            m_agc->reset();
            m_audioBuffer.clear();
            m_audioCapture->start();
        }
        else if (mode == Mode::Off) {
            m_audioCapture->stop();
            m_audioBuffer.clear();
        }
        
        m_mode = mode;
        qDebug() << "Mode changed to" << modeToString(mode);
        emit modeChanged(m_mode);
    }
}

void ApplicationController::onAudioDataReady(const QByteArray& data, int sampleRate) {
    // Добавляем в буфер
    m_audioBuffer.append(data);
    
    // Обрабатываем, пока в буфере достаточно данных
    while (m_audioBuffer.size() >= TARGET_CHUNK_SIZE) {
        QByteArray chunk = m_audioBuffer.left(TARGET_CHUNK_SIZE);
        m_audioBuffer.remove(0, TARGET_CHUNK_SIZE);
        
        // Применяем AGC
        QByteArray processedData = m_agc->process(chunk, sampleRate);
        
        // Рассчитываем RMS в dB для отладки
        const int16_t* samples = reinterpret_cast<const int16_t*>(processedData.constData());
        int numSamples = processedData.size() / sizeof(int16_t);
        
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            sum += samples[i] * samples[i];
        }
        double rms = std::sqrt(sum / numSamples);
        double db = (rms > 0) ? 20.0 * std::log10(rms / 32768.0) : -100.0;
        
        bool speech = m_agc->isSpeechDetected();
        
        qDebug() << "Audio:" << processedData.size() << "bytes,"
                 << "RMS:" << rms
                 << "(" << QString::number(db, 'f', 1) << "dB),"
                 << "Gain:" << m_agc->currentGain()
                 << "Speech:" << (speech ? "YES" : "NO");
    }
}

void ApplicationController::onAudioError(const QString& message) {
    qCritical() << "Audio error:" << message;
    emit errorOccurred(message);
}