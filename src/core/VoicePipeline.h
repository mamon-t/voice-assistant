#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

#include "config/AsrProfile.h"
#include "text/TextPostProcessor.h"

class ConfigManager;
class IRecognizer;
class SileroVad;

// Связка «микрофонный чанк -> VAD -> ASR -> постобработка -> готовый текст».
//
// Сделана отдельным классом намеренно: ApplicationController остаётся тонким
// (режимы, трей, инжектор), а вся речевая цепочка живёт здесь и тестируется
// отдельно (её же использует tools/vad_asr_test).
//
// Типовой usage в ApplicationController:
//
//   m_pipeline = std::make_unique<VoicePipeline>(VoicePipeline::loadSettings(m_config), this);
//   QString err;
//   if (!m_pipeline->initialize(&err)) { emit errorOccurred(err); return; }
//   connect(m_pipeline.get(), &VoicePipeline::textReady, this, &ApplicationController::onText);
//   ...
//   // в onAudioDataReady, после AGC:
//   m_pipeline->processAudio(processedData, sampleRate);
//   // при отпускании клавиши push-to-talk:
//   m_pipeline->flush();
class VoicePipeline : public QObject {
    Q_OBJECT

public:
    struct Settings {
        // VAD
        QString vadModelPath;
        float   vadThreshold    = 0.5f;
        float   vadMinSilence   = 0.5f;
        float   vadMinSpeech    = 0.25f;
        float   vadMaxSpeech    = 20.0f;
        int     vadWindowSize   = 512;
        int     vadNumThreads   = 1;
        float   vadBufferSeconds = 60.0f;

        // ASR
        AsrProfile asr;

        // Постобработка текста
        TextPostProcessor::Options text;

        // Скоринг подсказок. Формат sherpa-onnx позволяет задавать скор на фразу:
        // "фраза :1.00" (двоеточие обязательно ПОСЛЕ слов). Диктантные знаки
        // бустим слабее пользовательских слов: слишком сильный буст начинает
        // вставлять «точку» там, где её не произносили.
        float punctuationHotwordsScore = 2.0f;   // замер: ниже 1.75 слова теряются
    };

    // Собирает Settings из ConfigManager (settings.ini).
    static Settings loadSettings(const ConfigManager& config);

    explicit VoicePipeline(const Settings& settings, QObject* parent = nullptr);
    ~VoicePipeline() override;

    // Создаёт VAD и ASR. Возвращает false, если моделей нет на диске или
    // sherpa-onnx не смог инициализироваться; детали — в *error и в сигнале errorOccurred.
    bool initialize(QString* error = nullptr);

    bool isReady() const { return m_ready; }

    // PCM16 mono. Частота должна быть 16000 Гц (VAD в sherpa-onnx сам не ресемплит).
    void processAudio(const QByteArray& pcm16Mono, int sampleRate);

    // Вытолкнуть хвост текущей фразы (конец диктовки / отпускание клавиши PTT).
    void flush();

    // Сбросить состояние VAD и незадекодированный буфер ASR.
    void reset();

    // Подсказки пользователя. Реально применяются только для engine=transducer
    // в modified_beam_search; остальные движки пишут предупреждение в лог.
    //
    // К списку автоматически примешиваются диктантные знаки ("точка", "запятая",
    // "вопросительный знак", ...) если включена голосовая пунктуация и движок —
    // transducer: без буста ASR эти слова проглатывает, и пунктуация не появится.
    void setHotwords(const QStringList& words, float score);

    QString asrProfileName() const { return m_settings.asr.name; }

signals:
    // Готовая фраза после постобработки — её можно отдавать в инжектор или парсер команд.
    void textReady(const QString& text);

    // То, что выдал ASR до постобработки (для логов и отладки hotwords).
    void rawTextRecognized(const QString& text);

    void speechStarted();
    void speechEnded();
    void errorOccurred(const QString& message);

private:
    void onSpeechSegment(const QByteArray& pcm16, int sampleRate);
    void applyHotwords();

    Settings m_settings;
    std::unique_ptr<SileroVad> m_vad;
    std::unique_ptr<IRecognizer> m_asr;
    TextPostProcessor m_post;
    QStringList m_userHotwords;
    float m_hotwordsScore = 2.0f;
    bool m_hotwordsApplied = false;
    bool m_ready = false;
};
