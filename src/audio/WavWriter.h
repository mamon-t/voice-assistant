#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

// Запись PCM16 в WAV-файл.
//
// Зачем: без записи живого тракта (микрофон -> AGC -> VAD/ASR) невозможно
// отдебажить качество распознавания. Файл потом прогоняется офлайн:
//   ./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
// и сравнивается между профилями ASR, с AGC и без.
//
// Заголовок пишется сразу с нулевыми размерами и правится в close() —
// поэтому файл остаётся валидным даже после аварийного завершения:
// размеры можно пересчитать из фактической длины (так делает tools/vad_asr_test).
class WavWriter {
public:
    WavWriter();
    ~WavWriter();

    // bits пока только 16 — весь тракт проекта работает с PCM16
    bool open(const QString& path, int sampleRate, int channels = 1, int bits = 16);
    bool write(const QByteArray& pcm);
    void close();

    bool    isOpen() const { return m_file.isOpen(); }
    QString filePath() const { return m_file.fileName(); }
    qint64  dataBytes() const { return m_dataBytes; }
    int     sampleRate() const { return m_sampleRate; }
    int     channels() const { return m_channels; }
    double  seconds() const;
    QString lastError() const { return m_lastError; }

    // 44-байтный заголовок RIFF/WAVE PCM. Вынесено, чтобы его можно было
    // протестировать без файловой системы.
    static QByteArray makeHeader(int sampleRate, int channels, int bits, qint64 dataBytes);

    // Имя файла с отметкой времени: mic-check-20261003-042501.wav
    static QString defaultFileName(const QString& prefix = QStringLiteral("mic-check"));

private:
    bool patchHeader();

    QFile   m_file;
    int     m_sampleRate = 16000;
    int     m_channels   = 1;
    int     m_bits       = 16;
    qint64  m_dataBytes  = 0;
    QString m_lastError;
};
