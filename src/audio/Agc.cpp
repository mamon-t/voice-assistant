#include "Agc.h"
#include <cmath>
#include <QDebug>

Agc::Agc(QObject* parent)
    : QObject(parent)
{
}

Agc::~Agc() = default;

void Agc::calculateCoefficients(int sampleRate, int samplesPerChunk) {
    float attackSamples = sampleRate * m_attackTime / samplesPerChunk;
    float releaseSamples = sampleRate * m_releaseTime / samplesPerChunk;
    
    m_attackCoeff = std::exp(-1.0f / attackSamples);
    m_releaseCoeff = std::exp(-1.0f / releaseSamples);
    
    // Быстрый release для тишины (50 мс вместо 200 мс)
    float silenceReleaseSamples = sampleRate * 0.05f / samplesPerChunk;
    m_silenceReleaseCoeff = std::exp(-1.0f / silenceReleaseSamples);
}

QByteArray Agc::process(const QByteArray& inputData, int sampleRate) {
    if (inputData.isEmpty()) {
        return inputData;
    }
    
    int numSamples = inputData.size() / sizeof(int16_t);
    const int16_t* inputSamples = reinterpret_cast<const int16_t*>(inputData.constData());
    
    if (m_attackCoeff == 0.0f || m_releaseCoeff == 0.0f) {
        calculateCoefficients(sampleRate, numSamples);
    }
    
    // Конвертируем в float [-1.0, 1.0]
    QVector<float> floatSamples(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        floatSamples[i] = static_cast<float>(inputSamples[i]) / 32768.0f;
    }
    
    // Рассчитываем RMS
    double sum = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        sum += floatSamples[i] * floatSamples[i];
    }
    double rms = std::sqrt(sum / numSamples);
    
    // Рассчитываем текущий уровень в dB
    float currentDb = -100.0f;
    if (rms > 0.0) {
        currentDb = 20.0f * std::log10(rms);
    }
    
    // Noise gate: определяем, есть ли речь
    m_isSpeechDetected = (currentDb > m_noiseGateThresholdDb);
    
    // Рассчитываем необходимый gain
    float targetGain;
    if (m_isSpeechDetected) {
        // Есть речь - применяем AGC
        targetGain = std::pow(10.0f, (m_targetDb - currentDb) / 20.0f);
        targetGain = std::max(m_minGain, std::min(m_maxGain, targetGain));
    } else {
        // Тишина - минимальный gain
        targetGain = m_minGain;
    }
    
    // Применяем сглаживание (attack/release)
    if (targetGain > m_currentGain) {
        // Увеличиваем gain (attack)
        m_currentGain = m_attackCoeff * m_currentGain + (1.0f - m_attackCoeff) * targetGain;
    } else {
        // Уменьшаем gain (release)
        // В тишине используем БЫСТРЫЙ release
        float releaseCoeff = m_isSpeechDetected ? m_releaseCoeff : m_silenceReleaseCoeff;
        m_currentGain = releaseCoeff * m_currentGain + (1.0f - releaseCoeff) * targetGain;
    }
    
    // Применяем gain к данным
    for (int i = 0; i < numSamples; ++i) {
        floatSamples[i] *= m_currentGain;
        
        // Ограничиваем клиппинг
        if (floatSamples[i] > 1.0f) {
            floatSamples[i] = 1.0f;
        } else if (floatSamples[i] < -1.0f) {
            floatSamples[i] = -1.0f;
        }
    }
    
    // Конвертируем обратно в int16
    QByteArray outputData(numSamples * sizeof(int16_t), Qt::Uninitialized);
    int16_t* outputSamples = reinterpret_cast<int16_t*>(outputData.data());
    
    for (int i = 0; i < numSamples; ++i) {
        outputSamples[i] = static_cast<int16_t>(floatSamples[i] * 32767.0f);
    }
    
    return outputData;
}

void Agc::reset() {
    m_currentGain = 1.0f;
    m_attackCoeff = 0.0f;
    m_releaseCoeff = 0.0f;
    m_silenceReleaseCoeff = 0.0f;
    m_isSpeechDetected = false;
}

void Agc::setTargetDb(float targetDb) {
    m_targetDb = targetDb;
}

void Agc::setAttackTime(float seconds) {
    m_attackTime = seconds;
    m_attackCoeff = 0.0f;
}

void Agc::setReleaseTime(float seconds) {
    m_releaseTime = seconds;
    m_releaseCoeff = 0.0f;
}

void Agc::setMaxGain(float maxGain) {
    m_maxGain = maxGain;
}

void Agc::setMinGain(float minGain) {
    m_minGain = minGain;
}

void Agc::setNoiseGateThresholdDb(float thresholdDb) {
    m_noiseGateThresholdDb = thresholdDb;
}