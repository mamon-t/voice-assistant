#pragma once

#include <QDir>
#include <QFile>
#include <QString>

// Описание одной ASR-модели: откуда грузить и как декодировать.
// Профили живут в settings.ini, секции вида [asr_<имя>], активный — в [asr] active=.
//
// Проверенные на практике профили (см. README, раздел 1):
//   small-zipformer-ru        transducer  110 МБ  RTF 0.06   hotwords: да   ОСНОВНОЙ
//   GigaAM v3 RNN-T           transducer  167 МБ  RTF 0.27   hotwords: да   для шумного аудио
//   GigaAM v3 CTC             nemo-ctc    163 МБ  RTF 0.16   hotwords: нет
//   Whisper base              whisper     208 МБ  RTF 0.51   hotwords: нет  пунктуация + ITN + en
struct AsrProfile {
    enum class Engine {
        Unknown,
        Transducer,   // encoder + decoder + joiner (zipformer-ru, GigaAM RNN-T)
        NemoCtc,      // одна модель (GigaAM CTC, NeMo CTC)
        Whisper       // encoder + decoder (Whisper)
    };

    QString name;
    Engine  engine = Engine::Unknown;

    // transducer
    QString encoderPath;
    QString decoderPath;
    QString joinerPath;
    // nemo-ctc
    QString ctcModelPath;
    // общее
    QString tokensPath;

    // whisper
    QString language = QStringLiteral("ru");   // "" = автоопределение

    // hotwords (только transducer + modified_beam_search)
    QString bpeVocabPath;                       // словарь ssentencepiece для BPE-моделей
    QString modelingUnit;                       // "bpe" или пусто

    int     numThreads     = 2;
    QString decodingMethod = QStringLiteral("modified_beam_search");
    int     maxActivePaths = 4;
    float   hotwordsScore  = 2.0f;
    bool    debug          = false;

    static Engine engineFromString(const QString& s);
    static QString engineToString(Engine e);

    // Проверяет, что движок задан и все нужные файлы существуют на диске.
    bool isValid(QString* error = nullptr) const;
};

// ---------------------------------------------------------------------------

inline AsrProfile::Engine AsrProfile::engineFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("transducer") || v == QLatin1String("rnnt")
        || v == QLatin1String("zipformer") || v == QLatin1String("gigaam-rnnt")) {
        return Engine::Transducer;
    }
    if (v == QLatin1String("nemo-ctc") || v == QLatin1String("nemo_ctc")
        || v == QLatin1String("ctc") || v == QLatin1String("gigaam-ctc")) {
        return Engine::NemoCtc;
    }
    if (v == QLatin1String("whisper")) {
        return Engine::Whisper;
    }
    return Engine::Unknown;
}

inline QString AsrProfile::engineToString(AsrProfile::Engine e)
{
    switch (e) {
    case Engine::Transducer: return QStringLiteral("transducer");
    case Engine::NemoCtc:    return QStringLiteral("nemo-ctc");
    case Engine::Whisper:    return QStringLiteral("whisper");
    case Engine::Unknown:    break;
    }
    return QStringLiteral("unknown");
}

inline bool AsrProfile::isValid(QString* error) const
{
    const auto need = [&](const QString& path, const char* what) {
        if (path.isEmpty() || !QFile::exists(path)) {
            if (error) {
                *error = QStringLiteral(
                         "профиль '%1': %2 не найден: %3\n"
                         "  Модели скачиваются отдельно — см. docs/models.md "
                         "(wget из релизов k2-fsa/sherpa-onnx в ~/.voice_models).\n"
                         "  Диагностика: ./src/voice-assistant --check")
                         .arg(name, QString::fromLatin1(what), path);
            }
            return false;
        }
        return true;
    };

    switch (engine) {
    case Engine::Transducer:
        return need(encoderPath, "encoder")
            && need(decoderPath, "decoder")
            && need(joinerPath, "joiner")
            && need(tokensPath, "tokens");
    case Engine::NemoCtc:
        return need(ctcModelPath, "model")
            && need(tokensPath, "tokens");
    case Engine::Whisper:
        return need(encoderPath, "encoder")
            && need(decoderPath, "decoder")
            && need(tokensPath, "tokens");
    case Engine::Unknown:
        if (error) {
            *error = QStringLiteral("профиль '%1': неизвестный engine").arg(name);
        }
        return false;
    }
    return false;
}
