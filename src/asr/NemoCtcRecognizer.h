#pragma once

// NeMo-style CTC (одна модель) через sherpa-onnx C++ API.
//
// Целевая модель: sherpa-onnx-nemo-ctc-giga-am-v3-russian-2025-12-16 (GigaAM v3 CTC, MIT).
// Замер: 1200 мс на 7.68 с аудио (2 потока, RTF 0.16) — быстрее GigaAM RNN-T (1905 мс),
// но медленнее small-zipformer-ru (397 мс) и БЕЗ hotwords (CTC их не поддерживает).
//
// Конфиг: config.model_config.nemo_ctc.model + config.model_config.tokens.
// sherpa-onnx сам распознаёт GigaAM (IsGigaAM()) и подстраивает извлечение признаков
// (в частности n_fft=400) — вручную ничего делать не нужно.

#include "asr/IRecognizer.h"

#include <QString>
#include <memory>
#include <vector>

namespace sherpa_onnx {
namespace cxx {
class OfflineRecognizer;
}  // namespace cxx
}  // namespace sherpa_onnx

class NemoCtcRecognizer : public IRecognizer {
    Q_OBJECT

public:
    struct Options {
        int  numThreads = 2;
        bool debug      = false;
    };

    NemoCtcRecognizer(const QString& modelPath,
                      const QString& tokensPath,
                      int numThreads = 2,
                      QObject* parent = nullptr);
    NemoCtcRecognizer(const QString& modelPath,
                      const QString& tokensPath,
                      const Options& options,
                      QObject* parent = nullptr);
    ~NemoCtcRecognizer() override;

    bool initialize() override;
    void acceptWaveform(const QByteArray& audioData, int sampleRate) override;
    QString finalResult() override;
    void reset() override;
    void setHotwords(const QStringList& words, float score) override;
    bool isLoaded() const override;

    QString recognize(const QByteArray& pcm16, int sampleRate);

private:
    QString decodeBuffer();

    QString m_modelPath;
    QString m_tokensPath;
    Options m_options;

    bool m_loaded = false;
    std::unique_ptr<sherpa_onnx::cxx::OfflineRecognizer> m_recognizer;

    std::vector<float> m_buffer;
    int  m_bufferSampleRate = 16000;
    bool m_hotwordsWarned   = false;
};
