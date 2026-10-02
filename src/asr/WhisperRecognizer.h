#pragma once

// Whisper (offline ASR) через sherpa-onnx C++ API.
//
// Запасной движок (основной — ZipformerRecognizer под zipformer-ru).
// Whisper мультиязычный и сам расставляет пунктуацию и заглавные, но русский
// у него заметно хуже и в ~8 раз медленнее zipformer-ru.
//
// Замер на одной и той же русской фразе (2 потока, серверный CPU), через мои классы:
//   whisper-tiny int8 : "На бортех стар работает в любом поле вода"   1809 мс, RTF 0.25
//   whisper-base int8 : "На борте к стаработают в любом поле в вода"  3663 мс, RTF 0.51
//   small-zipformer-ru: "набор текста работает в любом поле ввода"     434 мс, RTF 0.06
//
// Пригодится, если понадобится диктовка на английском/других языках
// или готовая пунктуация ценой скорости.
//
// API (проверен по cxx-api.h v1.13.x, идентичен в master):
//   config.model_config.whisper.{encoder,decoder,language,task,tail_paddings}
//   config.model_config.tokens / num_threads / provider
//   OfflineRecognizer::Create(config) -> CreateStream() -> Decode(&stream) -> GetResult(&stream)

#include "asr/IRecognizer.h"

#include <QString>
#include <memory>
#include <vector>

namespace sherpa_onnx {
namespace cxx {
class OfflineRecognizer;
}  // namespace cxx
}  // namespace sherpa_onnx

class WhisperRecognizer : public IRecognizer {
    Q_OBJECT

public:
    struct Options {
        QString language   = QStringLiteral("ru");  // "" = автоопределение языка
        QString task       = QStringLiteral("transcribe");
        int     numThreads = 2;      // Селерон: 1..2
        int     tailPaddings = -1;   // -1 = по умолчанию в sherpa-onnx
        bool    debug      = false;
    };

    WhisperRecognizer(const QString& encoderPath,
                      const QString& decoderPath,
                      const QString& tokensPath,
                      int numThreads = 2,
                      QObject* parent = nullptr);
    WhisperRecognizer(const QString& encoderPath,
                      const QString& decoderPath,
                      const QString& tokensPath,
                      const Options& options,
                      QObject* parent = nullptr);
    ~WhisperRecognizer() override;

    // IRecognizer
    bool initialize() override;
    void acceptWaveform(const QByteArray& audioData, int sampleRate) override;
    QString finalResult() override;
    void reset() override;
    void setHotwords(const QStringList& words, float score) override;
    bool isLoaded() const override;

    // Дополнительно: распознать готовый сегмент (PCM16 mono) одним вызовом.
    // Принимает то, что приходит из SileroVad::speechSegmentReady().
    QString recognize(const QByteArray& pcm16, int sampleRate);

private:
    QString decodeBuffer();

    QString m_encoderPath;
    QString m_decoderPath;
    QString m_tokensPath;
    Options m_options;

    bool m_loaded = false;
    std::unique_ptr<sherpa_onnx::cxx::OfflineRecognizer> m_recognizer;

    std::vector<float> m_buffer;
    int  m_bufferSampleRate = 16000;
    bool m_hotwordsWarned   = false;
};
