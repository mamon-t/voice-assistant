#pragma once

// Silero VAD через sherpa-onnx ( VoiceActivityDetector ).
//
// ВАЖНО: отдельный ONNX Runtime здесь не нужен и не подключается.
// sherpa-onnx уже содержит Silero VAD, а libonnxruntime.so подтягивается
// как зависимость libsherpa-onnx-c-api.so.

#include "vad/IVad.h"

#include <QString>
#include <cstdint>
#include <memory>

// Прямое объявление: заголовок sherpa-onnx подключаем только в .cpp,
// чтобы не тянуть его во все TU проекта.
namespace sherpa_onnx {
namespace cxx {
class VoiceActivityDetector;
}  // namespace cxx
}  // namespace sherpa_onnx

class SileroVad : public IVad {
    Q_OBJECT

public:
    // Настройки детектора. Значения по умолчанию — как в silero_vad.onnx v4/v5.
    struct Options {
        float threshold          = 0.5f;   // порог "речь / не речь"
        float minSpeechDuration  = 0.25f;  // сек: короче — не речь
        float minSilenceDuration = 0.50f;  // сек: пауза, закрывающая сегмент
        float maxSpeechDuration  = 20.0f;  // сек: принудительная резка длинной фразы
        int   windowSize         = 512;    // сэмплов на окно (32 мс @ 16 кГц)
        int   numThreads         = 1;      // Селерон: строго 1
        float bufferSizeSeconds  = 60.0f;  // размер кольцевого буфера VAD
        bool  debug              = false;  // лог sherpa-onnx
    };

    explicit SileroVad(const QString& modelPath, QObject* parent = nullptr);
    SileroVad(const QString& modelPath, const Options& options, QObject* parent = nullptr);
    ~SileroVad() override;

    // IVad
    bool initialize() override;
    void processAudio(const QByteArray& audioData, int sampleRate) override;
    void reset() override;

    // Дополнительно (интерфейс IVad не меняем)
    void flush();                        // вытолкнуть хвост сегмента (для push-to-talk)
    bool isSpeaking() const { return m_isSpeaking; }
    int  sampleRate() const { return kSampleRate; }
    qint64 segmentsDetected() const { return m_segmentsDetected; }

signals:
    // speechStarted() / speechEnded() / errorOccurred() наследуются из IVad
    // и здесь НЕ переопределяются (сигналы Qt не виртуальны).

    // Готовый речевой сегмент: PCM16 mono, 16 кГц.
    // Именно его нужно отдавать в IRecognizer::acceptWaveform() — тогда
    // Нестриминговый ASR получает целую фразу, а не нарезку по чанкам.
    void speechSegmentReady(const QByteArray& pcm16, int sampleRate);

private:
    static constexpr int kSampleRate = 16000;

    QString m_modelPath;
    Options m_options;
    std::unique_ptr<sherpa_onnx::cxx::VoiceActivityDetector> m_vad;
    bool    m_isSpeaking       = false;
    qint64  m_segmentsDetected = 0;
};
