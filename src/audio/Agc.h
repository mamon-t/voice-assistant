#pragma once

#include <QObject>
#include <QByteArray>

class Agc : public QObject {
    Q_OBJECT

public:
    explicit Agc(QObject* parent = nullptr);
    ~Agc();

    QByteArray process(const QByteArray& inputData, int sampleRate);
    void reset();
    
    void setTargetDb(float targetDb);
    void setAttackTime(float seconds);
    void setReleaseTime(float seconds);
    void setMaxGain(float maxGain);
    void setMinGain(float minGain);
    void setNoiseGateThresholdDb(float thresholdDb);
    
    float currentGain() const { return m_currentGain; }
    bool isSpeechDetected() const { return m_isSpeechDetected; }

private:
    float m_targetDb = -15.0f;
    float m_attackTime = 0.02f;
    float m_releaseTime = 0.1f;
    float m_maxGain = 20.0f;
    float m_minGain = 0.01f;
    float m_noiseGateThresholdDb = -40.0f;
    
    float m_currentGain = 1.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;
    float m_silenceReleaseCoeff = 0.0f;  // ← НОВОЕ: быстрый release для тишины
    bool m_isSpeechDetected = false;
    
    void calculateCoefficients(int sampleRate, int samplesPerChunk);
};