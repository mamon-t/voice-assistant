#pragma once

// Offline TRANSDUCER (encoder + decoder + joiner) через sherpa-onnx C++ API.
//
// Класс модель-независимый: проверен на двух русских семействах, конфиг один и тот же —
//   config.model_config.transducer.{encoder,decoder,joiner} + tokens:
//
//   1) sherpa-onnx-small-zipformer-ru-2024-09-18   (110 МБ) — ОСНОВНОЙ, самый быстрый
//   2) sherpa-onnx-zipformer-ru-2024-09-18         (297 МБ) — на тесте не лучше и не быстрее малой
//   3) sherpa-onnx-nemo-transducer-giga-am-v2-russian-2025-04-19 (172 МБ) — GigaAM, MIT
//   4) sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16 (167 МБ) — GigaAM, MIT
//
// Замер на одной и той же русской фразе (7.16 с, 2 потока, серверный CPU;
// на Селероне время умножать на 3-5), текст во всех строках — через этот класс:
//   small-zipformer-ru, beam : 383-434 мс, RTF 0.05-0.06, текст точный
//   GigaAM v3 RNN-T,   beam  : 2209 мс,    RTF 0.31,     текст точный
//   GigaAM v3 RNN-T,   greedy: 1347 мс,    RTF 0.18      (официальный бинарник)
//   whisper-base int8        : 3663 мс,    RTF 0.51,     "На борте к стаработают..."
//   SenseVoice int8          : 664 мс,     RTF 0.12,     "RIVIE PALII IT RDS A GLSS..." (удалён)
//
// Два важных следствия:
//   * это TRANSDUCER, значит hotwords работают по-настоящему (контекстный граф
//     в modified_beam_search) — пункт ТЗ «улучшение распознавания по подсказкам»;
//   * GigaAM v3 точнее на шуме/нестандартной речи (callcenter, музыка, голосовые
//     сообщения: -30% WER по данным авторов), но в ~5 раз медленнее zipformer-ru.
//     Переключение между моделями — только пути к файлам, код не меняется.
//
// Ни одна из перечисленных моделей НЕ ставит знаки препинания и заглавные буквы.
// Пунктуацию умеют только GigaAM v3_e2e_ctc / v3_e2e_rnnt, а их в ONNX нет
// (на HF только pytorch_model.bin 442 МБ), и sherpa-onnx их не поддерживает.

#include "asr/IRecognizer.h"

#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

namespace sherpa_onnx {
namespace cxx {
class OfflineRecognizer;
}  // namespace cxx
}  // namespace sherpa_onnx

class TransducerRecognizer : public IRecognizer {
    Q_OBJECT

public:
    struct Options {
        // "modified_beam_search" — нужен для hotwords (единственный режим,
        //   в котором sherpa-onnx строит контекстный граф).
        // "greedy_search" — быстрее, но hotwords молча не применяются.
        QString decodingMethod = QStringLiteral("modified_beam_search");
        int     maxActivePaths = 4;
        int     numThreads     = 2;      // Селерон: 1..2
        float   hotwordsScore  = 2.0f;

        // Для BPE-моделей (у zipformer-ru токены — BPE-куски, в tokens.txt есть "▁").
        // Чтобы в hotwords можно было писать обычные слова, sherpa-onnx нужен
        // словарь ssentencepiece в текстовом виде "токен score" (НЕ бинарный bpe.model).
        // Оставь пустыми — тогда hotwords надо передавать уже разобранными на токены.
        QString modelingUnit;            // "bpe" или пусто
        QString bpeVocab;                // путь к словарю ssentencepiece или пусто

        bool    debug = false;
    };

    TransducerRecognizer(const QString& encoderPath,
                        const QString& decoderPath,
                        const QString& joinerPath,
                        const QString& tokensPath,
                        int numThreads = 2,
                        QObject* parent = nullptr);
    TransducerRecognizer(const QString& encoderPath,
                        const QString& decoderPath,
                        const QString& joinerPath,
                        const QString& tokensPath,
                        const Options& options,
                        QObject* parent = nullptr);
    ~TransducerRecognizer() override;

    // IRecognizer
    bool initialize() override;
    void acceptWaveform(const QByteArray& audioData, int sampleRate) override;
    QString finalResult() override;
    void reset() override;
    void setHotwords(const QStringList& words, float score) override;
    bool isLoaded() const override;

    // Дополнительно: распознать готовый сегмент (PCM16 mono) одним вызовом.
    QString recognize(const QByteArray& pcm16, int sampleRate);

    // Подсказки, которые сейчас в силе (для отладки/UI)
    QStringList hotwords() const { return m_hotwords; }

private:
    QString decodeBuffer();
    bool setOptionPath(const QString& path, const char* what);

    QString m_encoderPath;
    QString m_decoderPath;
    QString m_joinerPath;
    QString m_tokensPath;
    Options m_options;

    bool m_loaded = false;
    std::unique_ptr<sherpa_onnx::cxx::OfflineRecognizer> m_recognizer;

    QStringList m_hotwords;
    float m_hotwordsScore = 2.0f;

    std::vector<float> m_buffer;
    int  m_bufferSampleRate = 16000;
};
