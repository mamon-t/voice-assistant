#include "audio/AudioFileDecoder.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

// Подавление мусора в stderr при работе QAudioDecoder — см. decodeViaQt().
#ifdef HAVE_GLIB
#include <glib.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace {

constexpr int kTargetRate = 16000;

inline uint32_t rd32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline int32_t  rd32s(const char* p) { return static_cast<int32_t>(rd32(p)); }
inline float    rd32f(const char* p) { float v; std::memcpy(&v, p, 4); return v; }
inline double   rd64f(const char* p) { double v; std::memcpy(&v, p, 8); return v; }

// 24-битное знаковое LE (WAV PCM24)
inline int32_t rd24s(const char* p)
{
    uint32_t v = static_cast<uint8_t>(p[0])
               | (static_cast<uint8_t>(p[1]) << 8)
               | (static_cast<uint8_t>(p[2]) << 16);
    if (v & 0x800000u) {
        v |= 0xFF000000u;   // знак
    }
    return static_cast<int32_t>(v);
}

inline int16_t clampToInt16(double v)
{
    return static_cast<int16_t>(std::llrint(std::max(-32768.0, std::min(32767.0, v))));
}

// Расширения, которые осмысленно отдавать системному декодеру (GStreamer
// через QtMultimedia). Всё, чего здесь нет и что не RIFF/WAVE, считается
// RAW-потоком: у телефонных записей без контейнера расширения бывают какие
// угодно (.dat, .voc, вообще без имени).
const QStringList& qtDecoderExtensions()
{
    static const QStringList exts = {
        QStringLiteral("wav"),  // на случай экзотики внутри RIFF (ADPCM и т.п.)
        QStringLiteral("mp3"), QStringLiteral("mp2"),
        QStringLiteral("ogg"), QStringLiteral("oga"), QStringLiteral("opus"),
        QStringLiteral("flac"),
        QStringLiteral("m4a"), QStringLiteral("m4b"), QStringLiteral("mp4"),
        QStringLiteral("aac"),
        QStringLiteral("amr"), QStringLiteral("awb"),
        QStringLiteral("wma"), QStringLiteral("webm"), QStringLiteral("mka"),
        QStringLiteral("spx"), QStringLiteral("3gp"),
    };
    return exts;
}

#ifdef HAVE_GLIB
// ГРАБЛЯ: баг Qt5 GStreamer-бэкенда (проверено на Qt 5.15.8). При разборке
// QAudioDecoder конвейер получает flush, проба срабатывает с NULL-сэмплом, и
// Qt5 не проверяет его на null: в stderr улетает ТЫСЯЧИ строк вида
//   GStreamer-CRITICAL **: gst_sample_get_buffer: assertion 'GST_IS_SAMPLE'
// одним залпом за ~15 мс. На результат не влияет, но хоронит под собой
// полезную диагностику. Лечится только на стороне приложения: глушим
// G_LOG_LEVEL_CRITICAL домена "GStreamer" на время декодирования.
void silenceGStreamerCriticals(const gchar* /*domain*/, GLogLevelFlags /*levels*/,
                               const gchar* /*message*/, gpointer /*userData*/)
{
}

// RAII: живёт в decodeViaQt() ДО объявления QAudioDecoder, поэтому умирает
// ПОСЛЕ него (порядок разборки локальных объектов обратный) — подавление
// активно ровно в момент, когда декодер уничтожается и штормит.
struct GstCriticalSilencer {
    guint handlerId = 0;
    GstCriticalSilencer()
    {
        handlerId = g_log_set_handler("GStreamer",
                                      static_cast<GLogLevelFlags>(G_LOG_LEVEL_CRITICAL
                                                                  | G_LOG_FLAG_RECURSION),
                                      silenceGStreamerCriticals, nullptr);
    }
    ~GstCriticalSilencer()
    {
        if (handlerId != 0) {
            g_log_remove_handler("GStreamer", handlerId);
        }
    }
};
#endif

}  // namespace

// ---------------------------------------------------------------------------
// RawParams
// ---------------------------------------------------------------------------

AudioFileDecoder::RawFormat AudioFileDecoder::RawParams::formatFromString(const QString& s)
{
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("s8u") || v == QLatin1String("u8") || v == QLatin1String("s8")) {
        return RawFormat::S8U;
    }
    if (v == QLatin1String("f32le") || v == QLatin1String("f32") || v == QLatin1String("float32")) {
        return RawFormat::F32LE;
    }
    if (v == QLatin1String("alaw") || v == QLatin1String("a-law") || v == QLatin1String("g711a")
        || v == QLatin1String("pcma")) {
        return RawFormat::ALAW;
    }
    if (v == QLatin1String("ulaw") || v == QLatin1String("u-law") || v == QLatin1String("mulaw")
        || v == QLatin1String("g711u") || v == QLatin1String("pcmu")) {
        return RawFormat::ULAW;
    }
    return RawFormat::S16LE;   // s16le и всё неизвестное
}

QString AudioFileDecoder::RawParams::formatToString(RawFormat f)
{
    switch (f) {
    case RawFormat::S16LE: return QStringLiteral("s16le");
    case RawFormat::S8U:   return QStringLiteral("s8u");
    case RawFormat::F32LE: return QStringLiteral("f32le");
    case RawFormat::ALAW:  return QStringLiteral("alaw");
    case RawFormat::ULAW:  return QStringLiteral("ulaw");
    }
    return QStringLiteral("s16le");
}

QStringList AudioFileDecoder::RawParams::formatNames()
{
    return { QStringLiteral("s16le"), QStringLiteral("s8u"), QStringLiteral("f32le"),
             QStringLiteral("alaw"),  QStringLiteral("ulaw") };
}

int AudioFileDecoder::RawParams::bytesPerSample() const
{
    switch (format) {
    case RawFormat::S16LE: return 2;
    case RawFormat::F32LE: return 4;
    case RawFormat::S8U:
    case RawFormat::ALAW:
    case RawFormat::ULAW:  return 1;
    }
    return 2;
}

// ---------------------------------------------------------------------------
// G.711 (телефонные кодирования). Классические формулы из g711.c (Sun, 1986):
// таблицы на 256 значений не нужны, а контрольные значения совпадают
// с общепринятыми: alaw-тишина 0xD5 -> +8, ulaw-тишина 0xFF -> 0.
// ---------------------------------------------------------------------------

int16_t AudioFileDecoder::alawToLinear(uint8_t aVal)
{
    constexpr int kSignBit = 0x80;
    constexpr int kQuantMask = 0x0F;
    constexpr int kSegShift = 4;
    constexpr int kSegMask = 0x70;

    aVal ^= 0x55;
    int t = (aVal & kQuantMask) << 4;
    const int seg = (aVal & kSegMask) >> kSegShift;
    switch (seg) {
    case 0:
        t += 8;
        break;
    case 1:
        t += 0x108;
        break;
    default:
        t += 0x108;
        t <<= seg - 1;
        break;
    }
    return static_cast<int16_t>((aVal & kSignBit) ? t : -t);
}

int16_t AudioFileDecoder::ulawToLinear(uint8_t uVal)
{
    constexpr int kSignBit = 0x80;
    constexpr int kQuantMask = 0x0F;
    constexpr int kSegShift = 4;
    constexpr int kSegMask = 0x70;
    constexpr int kBias = 0x84;

    uVal = static_cast<uint8_t>(~uVal);
    int t = ((uVal & kQuantMask) << 3) + kBias;
    t <<= (uVal & kSegMask) >> kSegShift;
    return static_cast<int16_t>((uVal & kSignBit) ? (kBias - t) : (t - kBias));
}

// ---------------------------------------------------------------------------
// Передискретизация
// ---------------------------------------------------------------------------

void AudioFileDecoder::resampleTo16k(std::vector<int16_t>& mono, int srcRate)
{
    if (srcRate == kTargetRate || mono.empty() || srcRate <= 0) {
        return;
    }
    const double ratio = static_cast<double>(kTargetRate) / static_cast<double>(srcRate);
    const size_t dstFrames = static_cast<size_t>(std::llrint(double(mono.size()) * ratio));
    std::vector<int16_t> out(dstFrames);
    for (size_t i = 0; i < dstFrames; ++i) {
        const double srcPos = static_cast<double>(i) / ratio;
        const size_t i0 = static_cast<size_t>(srcPos);
        const size_t i1 = std::min(i0 + 1, mono.size() - 1);
        const double frac = srcPos - static_cast<double>(i0);
        const double v = static_cast<double>(mono[i0]) * (1.0 - frac)
                       + static_cast<double>(mono[i1]) * frac;
        out[i] = static_cast<int16_t>(std::llrint(v));
    }
    mono.swap(out);
}

// ---------------------------------------------------------------------------
// Публичное
// ---------------------------------------------------------------------------

QString AudioFileDecoder::fileDialogFilter()
{
    return QStringLiteral(
        "Аудиофайлы (*.wav *.mp3 *.ogg *.oga *.opus *.flac *.m4a *.mp4 *.aac "
        "*.amr *.awb *.wma *.raw *.pcm *.audio);;Все файлы (*)");
}

bool AudioFileDecoder::isRawExtension(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("raw") || ext == QLatin1String("pcm")
        || ext == QLatin1String("audio") || ext.isEmpty();
}

bool AudioFileDecoder::decode(const QString& path, const RawParams& rawParams,
                              Audio& out, QString* error)
{
    const auto fail = [error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("не могу открыть %1: %2").arg(path, f.errorString()));
    }

    const QByteArray head = f.peek(12);
    const bool isRiff = head.size() >= 12
        && std::memcmp(head.constData(), "RIFF", 4) == 0
        && std::memcmp(head.constData() + 8, "WAVE", 4) == 0;

    if (isRiff) {
        const QByteArray all = f.readAll();
        f.close();
        QString wavError;
        if (decodeWav(all, out, &wavError)) {
            return true;
        }
        // Экзотика внутри RIFF (ADPCM, битый заголовок) — пробуем системным
        // декодером, а если и он не смог, сообщаем исходную причину.
        QString qtError;
        if (decodeViaQt(path, out, &qtError)) {
            return true;
        }
        return fail(QStringLiteral("%1: %2 (системный декодер тоже не смог: %3)")
                        .arg(path, wavError, qtError));
    }

    const QString ext = QFileInfo(path).suffix().toLower();
    if (qtDecoderExtensions().contains(ext)) {
        f.close();
        return decodeViaQt(path, out, error);
    }

    // Всё остальное — RAW-поток: ни контейнера, ни параметров в файле нет.
    const QByteArray all = f.readAll();
    f.close();
    return decodeRaw(all, rawParams, out, error);
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

bool AudioFileDecoder::decodeWav(const QByteArray& data, Audio& out, QString* error)
{
    const auto fail = [error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    const char* p = data.constData();
    const int size = static_cast<int>(data.size());
    if (size < 44 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        return fail(QStringLiteral("не RIFF/WAVE"));
    }

    int formatTag = 0, channels = 0, sampleRate = 0, bits = 0;
    int dataOff = -1;
    uint32_t dataLen = 0;

    int pos = 12;
    while (pos + 8 <= size) {
        const char* id = p + pos;
        const uint32_t chunkSize = rd32(p + pos + 4);
        if (std::memcmp(id, "fmt ", 4) == 0 && pos + 8 + 16 <= size) {
            formatTag  = rd16(p + pos + 8);
            channels   = rd16(p + pos + 8 + 2);
            sampleRate = static_cast<int>(rd32(p + pos + 8 + 4));
            bits       = rd16(p + pos + 8 + 14);
            // WAVE_FORMAT_EXTENSIBLE (0xFFFE): настоящий тег лежит в GUID
            // подс формата (см. multichannel-записи профессиональных редакторов).
            if (formatTag == 0xFFFE && pos + 8 + 24 + 2 <= size) {
                formatTag = rd16(p + pos + 8 + 24);
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOff = pos + 8;
            // размер в заголовке может врать — режем по фактическому размеру файла
            dataLen = std::min<uint32_t>(chunkSize, static_cast<uint32_t>(size - pos - 8));
            break;
        }
        pos += 8 + static_cast<int>(chunkSize) + (chunkSize & 1u);
    }

    if (dataOff < 0) {
        return fail(QStringLiteral("нет чанка data"));
    }
    if (channels < 1 || sampleRate < 1) {
        return fail(QStringLiteral("битые fmt-данные (channels=%1, rate=%2)")
                        .arg(channels).arg(sampleRate));
    }

    // Сколько байт на один сэмпл одного канала и как его превращать в double
    // в диапазоне int16. Поддерживаем всё, что реально встречается в записях:
    // PCM 8/16/24/32, float 32/64, телефонные A-law/μ-law.
    int bytesPerSample = 0;
    std::function<double(const char*)> toDouble;
    switch (formatTag) {
    case 1:   // PCM
        switch (bits) {
        case 8:
            bytesPerSample = 1;
            toDouble = [](const char* q) { return (static_cast<double>(static_cast<uint8_t>(*q)) - 128.0) * 256.0; };
            break;
        case 16:
            bytesPerSample = 2;
            toDouble = [](const char* q) { return static_cast<double>(static_cast<int16_t>(rd16(q))); };
            break;
        case 24:
            bytesPerSample = 3;
            toDouble = [](const char* q) { return static_cast<double>(rd24s(q)) / 256.0; };
            break;
        case 32:
            bytesPerSample = 4;
            toDouble = [](const char* q) { return static_cast<double>(rd32s(q)) / 65536.0; };
            break;
        default:
            return fail(QStringLiteral("PCM %1 бит не поддерживается").arg(bits));
        }
        break;
    case 3:   // IEEE float
        if (bits == 32) {
            bytesPerSample = 4;
            toDouble = [](const char* q) { return static_cast<double>(rd32f(q)) * 32767.0; };
        } else if (bits == 64) {
            bytesPerSample = 8;
            toDouble = [](const char* q) { return rd64f(q) * 32767.0; };
        } else {
            return fail(QStringLiteral("float %1 бит не поддерживается").arg(bits));
        }
        break;
    case 6:   // A-law (G.711)
        bytesPerSample = 1;
        toDouble = [](const char* q) { return static_cast<double>(alawToLinear(static_cast<uint8_t>(*q))); };
        break;
    case 7:   // μ-law (G.711)
        bytesPerSample = 1;
        toDouble = [](const char* q) { return static_cast<double>(ulawToLinear(static_cast<uint8_t>(*q))); };
        break;
    default:
        return fail(QStringLiteral("формат WAV %1 (%2 бит) не поддерживается — "
                                   "это не PCM/float/G.711 (например, ADPCM); "
                                   "пересохраните файл в PCM WAV или MP3")
                        .arg(formatTag).arg(bits));
    }

    const int frameBytes = channels * bytesPerSample;
    const int frames = static_cast<int>(dataLen) / frameBytes;
    if (frames <= 0) {
        return fail(QStringLiteral("в чанке data нет ни одного кадра"));
    }

    std::vector<int16_t> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        double acc = 0.0;
        const char* base = p + dataOff + static_cast<size_t>(i) * frameBytes;
        for (int c = 0; c < channels; ++c) {
            acc += toDouble(base + c * bytesPerSample);
        }
        mono[static_cast<size_t>(i)] = clampToInt16(acc / channels);
    }

    out.sourceRate     = sampleRate;
    out.sourceChannels = channels;
    out.sourceFormat   = QStringLiteral("WAV %1 бит, format=%2")
                             .arg(bits).arg(formatTag);
    resampleTo16k(mono, sampleRate);
    out.samples.swap(mono);
    return true;
}

// ---------------------------------------------------------------------------
// RAW (PCM без контейнера)
// ---------------------------------------------------------------------------

bool AudioFileDecoder::decodeRaw(const QByteArray& data, const RawParams& pr,
                                 Audio& out, QString* error)
{
    const auto fail = [error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    if (pr.sampleRate < 1000 || pr.sampleRate > 384000) {
        return fail(QStringLiteral("RAW: частота %1 Гц вне диапазона 1000..384000")
                        .arg(pr.sampleRate));
    }
    if (pr.channels < 1 || pr.channels > 8) {
        return fail(QStringLiteral("RAW: число каналов %1 вне диапазона 1..8").arg(pr.channels));
    }

    const int bps = pr.bytesPerSample();
    const int frameBytes = bps * pr.channels;
    const int frames = data.size() / frameBytes;
    if (frames <= 0) {
        return fail(QStringLiteral("RAW: файл слишком мал (%1 Б) для параметров "
                                   "%2 Гц, %3 кан., %4")
                        .arg(data.size()).arg(pr.sampleRate).arg(pr.channels)
                        .arg(RawParams::formatToString(pr.format)));
    }

    const char* p = data.constData();
    std::vector<int16_t> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        double acc = 0.0;
        const char* base = p + static_cast<size_t>(i) * frameBytes;
        for (int c = 0; c < pr.channels; ++c) {
            const char* q = base + c * bps;
            switch (pr.format) {
            case RawFormat::S16LE:
                acc += static_cast<double>(static_cast<int16_t>(rd16(q)));
                break;
            case RawFormat::S8U:
                acc += (static_cast<double>(static_cast<uint8_t>(*q)) - 128.0) * 256.0;
                break;
            case RawFormat::F32LE:
                acc += static_cast<double>(rd32f(q)) * 32767.0;
                break;
            case RawFormat::ALAW:
                acc += static_cast<double>(alawToLinear(static_cast<uint8_t>(*q)));
                break;
            case RawFormat::ULAW:
                acc += static_cast<double>(ulawToLinear(static_cast<uint8_t>(*q)));
                break;
            }
        }
        mono[static_cast<size_t>(i)] = clampToInt16(acc / pr.channels);
    }

    out.sourceRate     = pr.sampleRate;
    out.sourceChannels = pr.channels;
    out.sourceFormat   = QStringLiteral("RAW %1, %2 Гц, %3 кан.")
                             .arg(RawParams::formatToString(pr.format))
                             .arg(pr.sampleRate).arg(pr.channels);
    resampleTo16k(mono, pr.sampleRate);
    out.samples.swap(mono);
    return true;
}

// ---------------------------------------------------------------------------
// Сжатые форматы: QAudioDecoder (в Linux — GStreamer)
// ---------------------------------------------------------------------------

bool AudioFileDecoder::decodeViaQt(const QString& path, Audio& out, QString* error)
{
    const auto fail = [error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

#ifdef HAVE_GLIB
    // Объявлен ДО decoder: см. GstCriticalSilencer.
    GstCriticalSilencer gstSilencer;
#endif
    QAudioDecoder decoder;

    // Просим сразу то, что нужно конвейеру: GStreamer сам вставит
    // audioconvert/audioresample. Если бэкенд запрос проигнорировал —
    // буферы всё равно конвертируются вручную по фактическому формату.
    QAudioFormat want;
    want.setSampleRate(kTargetRate);
    want.setChannelCount(1);
    want.setSampleSize(16);
    want.setCodec(QStringLiteral("audio/pcm"));
    want.setSampleType(QAudioFormat::SignedInt);
    decoder.setAudioFormat(want);
    decoder.setSourceFilename(path);

    std::vector<int16_t> acc;
    int accRate = 0;          // частота накопленных сэмплов (0 = ещё нет)
    QString decodeError;

    QEventLoop loop;
    // Watchdog БЕЗДЕЙСТВИЯ: битый/обрезанный файл может не дать ни finished(),
    // ни error() — декодер просто встаёт (поймано живым тестом на обрезанном
    // MP3: висели 10 минут). Декодирование идёт в разы быстрее реального
    // времени и буферы поступают непрерывно, поэтому 15 с без единого буфера —
    // гарантированный признак зависания, а не медленной работы. Таймер
    // перезапускается с каждым буфером; для больших файлов ложно не сработает.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(15000);
    QObject::connect(&watchdog, &QTimer::timeout, [&]() {
        decodeError = QStringLiteral(
            "системный декодер завис (15 с без данных) — файл битый или формат "
            "не поддерживается");
        loop.quit();
    });

    auto appendMono = [&acc, &accRate](const int16_t* data, int frames, int rate) {
        if (frames <= 0) {
            return;
        }
        if (accRate == 0) {
            accRate = rate;
        }
        if (rate != accRate) {
            // практически не встречается: приводим накопленное к 16 кГц и
            // продолжаем накапливать уже в целевой частоте
            resampleTo16k(acc, accRate);
            accRate = kTargetRate;
            std::vector<int16_t> chunk(data, data + frames);
            resampleTo16k(chunk, rate);
            acc.insert(acc.end(), chunk.begin(), chunk.end());
            return;
        }
        acc.insert(acc.end(), data, data + frames);
    };

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &decoder, [&]() {
        watchdog.start();   // данные идут — зависания нет
        while (true) {
            const QAudioBuffer buf = decoder.read();
            if (!buf.isValid()) {
                break;
            }
            const QAudioFormat fmt = buf.format();
            const int ch = std::max(1, fmt.channelCount());
            const int frames = buf.frameCount();
            const int rate = fmt.sampleRate() > 0 ? fmt.sampleRate() : kTargetRate;

            std::vector<int16_t> mono(static_cast<size_t>(frames));
            bool understood = true;

            if (fmt.sampleSize() == 16 && fmt.sampleType() == QAudioFormat::SignedInt) {
                const int16_t* src = buf.data<int16_t>();
                for (int i = 0; i < frames; ++i) {
                    int32_t sum = 0;
                    for (int c = 0; c < ch; ++c) {
                        sum += src[static_cast<size_t>(i) * ch + c];
                    }
                    mono[static_cast<size_t>(i)] = static_cast<int16_t>(sum / ch);
                }
            } else if (fmt.sampleSize() == 8) {
                const uint8_t* src = buf.data<uint8_t>();
                for (int i = 0; i < frames; ++i) {
                    int32_t sum = 0;
                    for (int c = 0; c < ch; ++c) {
                        const uint8_t v = src[static_cast<size_t>(i) * ch + c];
                        sum += fmt.sampleType() == QAudioFormat::UnSignedInt
                                   ? (static_cast<int>(v) - 128) << 8
                                   : static_cast<int>(static_cast<int8_t>(v)) << 8;
                    }
                    mono[static_cast<size_t>(i)] = static_cast<int16_t>(sum / ch);
                }
            } else if (fmt.sampleSize() == 32 && fmt.sampleType() == QAudioFormat::Float) {
                const float* src = buf.data<float>();
                for (int i = 0; i < frames; ++i) {
                    double sum = 0.0;
                    for (int c = 0; c < ch; ++c) {
                        sum += src[static_cast<size_t>(i) * ch + c];
                    }
                    mono[static_cast<size_t>(i)] = clampToInt16(sum / ch * 32767.0);
                }
            } else if (fmt.sampleSize() == 32 && fmt.sampleType() == QAudioFormat::SignedInt) {
                const int32_t* src = buf.data<int32_t>();
                for (int i = 0; i < frames; ++i) {
                    double sum = 0.0;
                    for (int c = 0; c < ch; ++c) {
                        sum += static_cast<double>(src[static_cast<size_t>(i) * ch + c]) / 65536.0;
                    }
                    mono[static_cast<size_t>(i)] = clampToInt16(sum / ch);
                }
            } else {
                understood = false;
            }

            if (!understood) {
                decodeError = QStringLiteral(
                    "системный декодер вернул неподдерживаемый формат: %1 бит, тип %2")
                                  .arg(fmt.sampleSize()).arg(int(fmt.sampleType()));
                loop.quit();
                return;
            }
            appendMono(mono.data(), frames, rate);
        }
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    // error() в Qt5 перегружен (сигнал + геттер) — выбираем сигнал явно.
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
                     &decoder, [&]() {
        decodeError = decoder.errorString().isEmpty()
            ? QStringLiteral("ошибка системного декодера (код %1)").arg(int(decoder.error()))
            : decoder.errorString();
        loop.quit();
    });

    watchdog.start();
    decoder.start();
    loop.exec();
    watchdog.stop();
    // decoder.stop() НЕ вызываем: у Qt5 GStreamer-бэкенда stop() после
    // finished() даёт flush конвейера, и пробы печатают лавину
    // «gst_sample_get_buffer: assertion 'GST_IS_SAMPLE' failed» (поймано
    // живым тестом). decoder — стековый объект: деструктор сам гасит конвейер.

    if (!decodeError.isEmpty()) {
        return fail(QStringLiteral("%1: %2").arg(path, decodeError));
    }
    if (acc.empty()) {
        return fail(QStringLiteral("%1: декодер не вернул ни одного сэмпла — формат не "
                                   "поддерживается или нужен gstreamer-плагин "
                                   "(sudo apt install gstreamer1.0-plugins-good "
                                   "gstreamer1.0-plugins-ugly gstreamer1.0-libav)")
                        .arg(path));
    }

    out.sourceRate     = accRate;
    out.sourceChannels = 1;   // после домикширования
    out.sourceFormat   = QStringLiteral("системный декодер (QtMultimedia/GStreamer)");
    resampleTo16k(acc, accRate);
    out.samples.swap(acc);
    return true;
}
