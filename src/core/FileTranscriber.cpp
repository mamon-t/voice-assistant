#include "core/FileTranscriber.h"

#include <QDebug>
#include <QElapsedTimer>

#include <algorithm>
#include <cstdint>

FileTranscriber::FileTranscriber(QString path,
                                 VoicePipeline::Settings settings,
                                 AudioFileDecoder::RawParams rawParams,
                                 QObject* parent)
    : QObject(parent)
    , m_path(std::move(path))
    , m_settings(std::move(settings))
    , m_raw(rawParams)
{
}

FileTranscriber::~FileTranscriber() = default;

void FileTranscriber::setUserHotwords(const QStringList& words, float score)
{
    m_hotwords     = words;
    m_hotwordsScore = score;
}

void FileTranscriber::run()
{
    QElapsedTimer wall;
    wall.start();

    // --- 1. Декодирование файла ------------------------------------------------
    emit progress(2, QStringLiteral("Читаю аудиофайл…"));

    AudioFileDecoder::Audio audio;
    QString err;
    if (!AudioFileDecoder::decode(m_path, m_raw, audio, &err)) {
        emit failed(err);
        return;
    }
    if (m_cancel.load()) {
        emit cancelled();
        return;
    }
    if (audio.samples.empty()) {
        emit failed(QStringLiteral("%1: в файле нет аудио (0 сэмплов)").arg(m_path));
        return;
    }

    qInfo().noquote() << QStringLiteral("Разбор файла: %1 — %2 с, %3")
                             .arg(m_path)
                             .arg(audio.seconds(), 0, 'f', 1)
                             .arg(audio.sourceFormat);
    emit progress(8, QStringLiteral("Аудио %1 с (%2) — загружаю модель…")
                         .arg(audio.seconds(), 0, 'f', 1)
                         .arg(audio.sourceFormat));

    // --- 2. Конвейер (создан В РАБОЧЕМ потоке — там же и умрёт) ----------------
    VoicePipeline pipeline(m_settings);

    QString pipelineError;
    QObject::connect(&pipeline, &VoicePipeline::errorOccurred,
                     [&pipelineError](const QString& m) { pipelineError = m; });

    QStringList segments;
    QObject::connect(&pipeline, &VoicePipeline::textReady,
                     [this, &segments](const QString& text) {
        const QString s = text.trimmed();
        if (s.isEmpty()) {
            return;
        }
        segments << s;
        emit segmentRecognized(segments.size(), s);
    });

    if (!pipeline.initialize(&err)) {
        emit failed(QStringLiteral("Модель не запустилась: %1").arg(err));
        return;
    }
    if (m_cancel.load()) {
        emit cancelled();
        return;
    }
    if (!m_hotwords.isEmpty()) {
        pipeline.setHotwords(m_hotwords, m_hotwordsScore);
    }

    // --- 3. Подача аудио окнами по 512 сэмплов (32 мс), как в vad_asr_test ----
    constexpr int kWindowSamples = 512;
    const int windowBytes = kWindowSamples * static_cast<int>(sizeof(int16_t));
    const int totalBytes  = static_cast<int>(audio.samples.size() * sizeof(int16_t));
    const char* raw = reinterpret_cast<const char*>(audio.samples.data());

    int lastPercent = -1;
    int chunkIdx = 0;
    for (int off = 0; off < totalBytes; off += windowBytes, ++chunkIdx) {
        // Раз в 16 окон (~0.5 с аудио): проверка отмены и шаг прогресса.
        if ((chunkIdx % 16) == 0) {
            if (m_cancel.load()) {
                emit cancelled();
                return;
            }
            const int percent = 12 + static_cast<int>(
                87.0 * static_cast<double>(off) / static_cast<double>(std::max(1, totalBytes)));
            if (percent != lastPercent) {
                lastPercent = percent;
                emit progress(std::min(percent, 99),
                              QStringLiteral("Распознаю: %1 из %2 с")
                                  .arg(static_cast<double>(off / 2) / 16000.0, 0, 'f', 0)
                                  .arg(audio.seconds(), 0, 'f', 0));
            }
        }
        const int n = std::min(windowBytes, totalBytes - off);
        pipeline.processAudio(QByteArray(raw + off, n), 16000);
    }
    pipeline.flush();

    if (m_cancel.load()) {
        emit cancelled();
        return;
    }
    if (!pipelineError.isEmpty()) {
        qWarning().noquote() << QStringLiteral("Разбор файла: конвейер сообщил: %1")
                                    .arg(pipelineError);
    }

    emit progress(100, QStringLiteral("Готово"));
    emit succeeded(segments.join(QLatin1Char('\n')), segments.size(),
                   audio.seconds(), static_cast<double>(wall.elapsed()));
}
