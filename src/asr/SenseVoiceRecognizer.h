#pragma once

#include "IRecognizer.h"
#include <QString>
#include <memory>
#include <vector>

namespace sherpa_onnx {
    class OfflineRecognizer;
    class OfflineStream;
}

class SenseVoiceRecognizer : public IRecognizer {
    Q_OBJECT

public:
    SenseVoiceRecognizer(const QString& modelPath,
                         const QString& tokensPath,
                         int numThreads = 2,
                         QObject* parent = nullptr);
    ~SenseVoiceRecognizer();

    bool initialize() override;
    void acceptWaveform(const QByteArray& audioData, int sampleRate) override;
    QString finalResult() override;
    void reset() override;
    void setHotwords(const QStringList& words, float score) override;
    bool isLoaded() const override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
    
    QString m_modelPath;
    QString m_tokensPath;
    int m_numThreads;
    bool m_loaded = false;
    
    std::vector<std::unique_ptr<sherpa_onnx::OfflineStream>> m_streams;
};