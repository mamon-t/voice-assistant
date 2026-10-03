#include "audio/WavWriter.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>

namespace {

void put32(QByteArray& dst, int offset, quint32 value)
{
    qToLittleEndian<quint32>(value, reinterpret_cast<uchar*>(dst.data()) + offset);
}

void put16(QByteArray& dst, int offset, quint16 value)
{
    qToLittleEndian<quint16>(value, reinterpret_cast<uchar*>(dst.data()) + offset);
}

}  // namespace

WavWriter::WavWriter() = default;

WavWriter::~WavWriter()
{
    close();
}

QByteArray WavWriter::makeHeader(int sampleRate, int channels, int bits, qint64 dataBytes)
{
    const int byteRate   = sampleRate * channels * bits / 8;
    const int blockAlign = channels * bits / 8;
    const quint32 data   = static_cast<quint32>(dataBytes);
    const quint32 riff   = data + 36;

    QByteArray h(44, '\0');
    h.replace(0, 4, "RIFF");
    put32(h, 4, riff);
    h.replace(8, 4, "WAVE");

    h.replace(12, 4, "fmt ");
    put32(h, 16, 16);                       // размер блока fmt
    put16(h, 20, 1);                        // PCM
    put16(h, 22, static_cast<quint16>(channels));
    put32(h, 24, static_cast<quint32>(sampleRate));
    put32(h, 28, static_cast<quint32>(byteRate));
    put16(h, 32, static_cast<quint16>(blockAlign));
    put16(h, 34, static_cast<quint16>(bits));

    h.replace(36, 4, "data");
    put32(h, 40, data);
    return h;
}

QString WavWriter::defaultFileName(const QString& prefix)
{
    return QStringLiteral("%1-%2.wav")
        .arg(prefix, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

bool WavWriter::open(const QString& path, int sampleRate, int channels, int bits)
{
    if (m_file.isOpen()) {
        close();
    }
    if (bits != 16) {
        m_lastError = QStringLiteral("WavWriter: поддерживается только 16 бит, запрошено %1").arg(bits);
        return false;
    }
    if (sampleRate <= 0 || channels <= 0) {
        m_lastError = QStringLiteral("WavWriter: некорректные sampleRate/channels");
        return false;
    }
    if (path.isEmpty()) {
        m_lastError = QStringLiteral("WavWriter: не задан путь");
        return false;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = QStringLiteral("WavWriter: не могу открыть %1: %2")
                          .arg(path, m_file.errorString());
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels   = channels;
    m_bits       = bits;
    m_dataBytes  = 0;

    const QByteArray header = makeHeader(sampleRate, channels, bits, 0);
    if (m_file.write(header) != header.size()) {
        m_lastError = QStringLiteral("WavWriter: не смог записать заголовок в %1").arg(path);
        m_file.close();
        return false;
    }
    return true;
}

bool WavWriter::write(const QByteArray& pcm)
{
    if (!m_file.isOpen()) {
        m_lastError = QStringLiteral("WavWriter::write: файл не открыт");
        return false;
    }
    if (pcm.isEmpty()) {
        return true;
    }
    const qint64 n = m_file.write(pcm);
    if (n != pcm.size()) {
        m_lastError = QStringLiteral("WavWriter: записано %1 из %2 байт: %3")
                          .arg(n).arg(pcm.size()).arg(m_file.errorString());
        return false;
    }
    m_dataBytes += n;
    return true;
}

bool WavWriter::patchHeader()
{
    if (!m_file.isOpen()) {
        return false;
    }
    if (!m_file.seek(0)) {
        m_lastError = QStringLiteral("WavWriter: не смог вернуться в начало файла");
        return false;
    }
    const QByteArray header = makeHeader(m_sampleRate, m_channels, m_bits, m_dataBytes);
    if (m_file.write(header) != header.size()) {
        m_lastError = QStringLiteral("WavWriter: не смог обновить заголовок");
        return false;
    }
    return m_file.seek(m_dataBytes + 44);
}

void WavWriter::close()
{
    if (!m_file.isOpen()) {
        return;
    }
    patchHeader();
    m_file.flush();
    m_file.close();
}

double WavWriter::seconds() const
{
    const int bytesPerSecond = m_sampleRate * m_channels * m_bits / 8;
    if (bytesPerSecond <= 0) {
        return 0.0;
    }
    return static_cast<double>(m_dataBytes) / static_cast<double>(bytesPerSecond);
}
