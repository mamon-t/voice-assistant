#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>

#include "audio/AudioFileDecoder.h"
#include "core/VoicePipeline.h"

// Разбор одного аудиофайла: декодирование (AudioFileDecoder) -> боевой
// конвейер (SileroVAD -> ASR -> постобработка) -> готовый текст.
//
// Зачем отдельный объект и свой поток. Часовой звонок разбирается минуты;
// заморозить на это время GUI нельзя, поэтому FileTranscriber живёт в
// QThread (см. ApplicationController::startFileTranscription). Диктующий
// VoicePipeline при этом не трогается: модели грузятся независимо, плата —
// временно двойной расход ОЗУ на время разбора файла.
//
// Поток создания: конструктор и setUserHotwords() вызываются в главном
// потоке, затем moveToThread() и run() — уже в рабочем. Все сигналы
// уходят в главный поток очередью (автоматически: получатель живёт там).
//
// Память: файл декодируется ЦЕЛИКОМ до подачи в конвейер (моно 16 кГц
// int16 = ~115 МБ на час аудио). Для телефонных записей (8 кГц, минуты)
// это единицы мегабайт; стриминговое декодирование — возможное улучшение
// на будущее.
class FileTranscriber : public QObject {
    Q_OBJECT

public:
    FileTranscriber(QString path,
                    VoicePipeline::Settings settings,
                    AudioFileDecoder::RawParams rawParams,
                    QObject* parent = nullptr);
    ~FileTranscriber() override;

    // Подсказки пользователя (имена, термины) — как в диктовке, но БЕЗ
    // фраз команд: буст «удали слово» в часовом звонке ни к чему.
    void setUserHotwords(const QStringList& words, float score);

    // Потокобезопасно: флаг проверяется в цикле подачи аудио (~раз в 0.5 с).
    void requestCancel() { m_cancel.store(true); }
    bool isCancelRequested() const { return m_cancel.load(); }

public slots:
    // Основной цикл. Вызывается ОДИН раз из рабочего потока
    // (connect QThread::started -> run).
    void run();

signals:
    // 0..100 + человекочитаемая стадия — для QProgressDialog.
    void progress(int percent, const QString& stage);
    // Каждый распознанный сегмент, как приходит (для логов и живого UI).
    void segmentRecognized(int index, const QString& text);
    // Весь текст (сегменты через '\n'), число сегментов, длительность и wall-time.
    void succeeded(const QString& text, int segments, double audioSeconds, double wallMs);
    void failed(const QString& error);
    void cancelled();

private:
    QString m_path;
    VoicePipeline::Settings m_settings;
    AudioFileDecoder::RawParams m_raw;
    QStringList m_hotwords;
    float m_hotwordsScore = 2.0f;
    std::atomic<bool> m_cancel{false};
};
