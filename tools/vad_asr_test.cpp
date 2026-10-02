// vad-asr-test — прогон WAV через БОЕВОЙ путь: ConfigManager -> VoicePipeline
// (SileroVad -> IRecognizer -> TextPostProcessor), без UI и микрофона.
//
//   vad-asr-test <file.wav> --config <settings.ini> [--profile <имя>] [--hotwords <file>]
//                             [--threads N] [--no-punct]
//
//   vad-asr-test <file.wav> <silero_vad.onnx> transducer <enc> <dec> <joiner> <tokens>
//                          [hotwords-file] [bpe-vocab]
//   vad-asr-test <file.wav> <silero_vad.onnx> nemo-ctc <model> <tokens>
//   vad-asr-test <file.wav> <silero_vad.onnx> whisper <enc> <dec> <tokens> [language]
//
// WAV: PCM 16 бит, любое число каналов и любая частота (инструмент сам домикширует
// в моно и передискретизирует в 16 кГц).

#include "config/ConfigManager.h"
#include "core/VoicePipeline.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kTargetRate = 16000;

struct WavData {
    std::vector<int16_t> mono;   // уже моно и уже 16 кГц
    int    sampleRate = 0;       // исходная частота (для справки)
    int    channels   = 0;
    int    bits       = 0;
    double seconds    = 0.0;
};

inline uint32_t rd32(const char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const char* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

bool readWav(const QString& path, WavData& out, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("не могу открыть %1").arg(path);
        return false;
    }
    const QByteArray all = f.readAll();
    const char* p = all.constData();
    const int size = static_cast<int>(all.size());

    if (size < 44 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        if (error) *error = QStringLiteral("%1: не RIFF/WAVE").arg(path);
        return false;
    }

    int channels = 0, sampleRate = 0, bits = 0, format = 0;
    int dataOff = -1;
    uint32_t dataLen = 0;

    int pos = 12;
    while (pos + 8 <= size) {
        const char* id = p + pos;
        const uint32_t chunkSize = rd32(p + pos + 4);
        if (std::memcmp(id, "fmt ", 4) == 0 && pos + 8 + 16 <= size) {
            format     = rd16(p + pos + 8);
            channels   = rd16(p + pos + 8 + 2);
            sampleRate = static_cast<int>(rd32(p + pos + 8 + 4));
            bits       = rd16(p + pos + 8 + 14);
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOff = pos + 8;
            // размер в заголовке может врать — режем по фактическому размеру файла
            dataLen = std::min<uint32_t>(chunkSize, static_cast<uint32_t>(size - pos - 8));
            break;
        }
        pos += 8 + static_cast<int>(chunkSize) + (chunkSize & 1u);
    }

    if (dataOff < 0) {
        if (error) *error = QStringLiteral("%1: нет чанка data").arg(path);
        return false;
    }
    if (format != 1 || bits != 16) {
        if (error) *error = QStringLiteral("%1: нужен PCM 16 бит (format=%2, bits=%3)")
                                .arg(path).arg(format).arg(bits);
        return false;
    }
    if (channels < 1 || sampleRate < 1) {
        if (error) *error = QStringLiteral("%1: битые fmt-данные").arg(path);
        return false;
    }

    const int frameBytes = channels * 2;
    const int frames = static_cast<int>(dataLen) / frameBytes;

    std::vector<int16_t> mono(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        int32_t acc = 0;
        for (int c = 0; c < channels; ++c) {
            acc += static_cast<int16_t>(rd16(p + dataOff + (i * channels + c) * 2));
        }
        mono[static_cast<size_t>(i)] = static_cast<int16_t>(acc / channels);
    }

    out.channels   = channels;
    out.sampleRate = sampleRate;
    out.bits       = bits;

    if (sampleRate == kTargetRate) {
        out.mono = std::move(mono);
    } else {
        const double ratio = static_cast<double>(kTargetRate) / static_cast<double>(sampleRate);
        const size_t dstFrames = static_cast<size_t>(std::llrint(mono.size() * ratio));
        out.mono.resize(dstFrames);
        for (size_t i = 0; i < dstFrames; ++i) {
            const double srcPos = static_cast<double>(i) / ratio;
            const size_t i0 = static_cast<size_t>(srcPos);
            const size_t i1 = std::min(i0 + 1, mono.size() - 1);
            const double frac = srcPos - static_cast<double>(i0);
            const double v = static_cast<double>(mono[i0]) * (1.0 - frac)
                           + static_cast<double>(mono[i1]) * frac;
            out.mono[i] = static_cast<int16_t>(std::llrint(v));
        }
    }

    out.seconds = static_cast<double>(out.mono.size()) / static_cast<double>(kTargetRate);
    return true;
}

QStringList readLines(const QString& path)
{
    QStringList out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QTextStream in(&f);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            out << line;
        }
    }
    return out;
}

void usage(const QString& prog)
{
    std::fprintf(stderr,
        "Usage:\n"
        "  %s <file.wav> --config <settings.ini> [--profile <name>] [--hotwords <file>]\n"
        "                [--threads N] [--no-punct]\n"
        "  %s <file.wav> <silero_vad.onnx> transducer <enc> <dec> <joiner> <tokens>"
        " [hotwords-file] [bpe-vocab]\n"
        "  %s <file.wav> <silero_vad.onnx> nemo-ctc <model> <tokens>\n"
        "  %s <file.wav> <silero_vad.onnx> whisper <enc> <dec> <tokens> [language]\n",
        qPrintable(prog), qPrintable(prog), qPrintable(prog), qPrintable(prog));
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();

    if (a.size() < 4) {
        usage(a.value(0));
        return 2;
    }

    const QString wavPath = a.at(1);

    WavData wav;
    QString wavError;
    if (!readWav(wavPath, wav, &wavError)) {
        std::fprintf(stderr, "WAV error: %s\n", qPrintable(wavError));
        return 1;
    }
    std::printf("WAV : %s\n      %d Hz, %d ch, %d bit -> %.2f s @ 16 kHz mono\n",
                qPrintable(wavPath), wav.sampleRate, wav.channels, wav.bits, wav.seconds);

    VoicePipeline::Settings settings;
    QString hotwordsFile;

    if (a.at(2) == QLatin1String("--config")) {
        // --------- БОЕВОЙ РЕЖИМ: всё из settings.ini ---------
        const ConfigManager cfg(a.at(3));
        settings = VoicePipeline::loadSettings(cfg);

        for (int i = 4; i < a.size(); ++i) {
            const QString& k = a.at(i);
            if (k == QLatin1String("--profile") && i + 1 < a.size()) {
                settings.asr = cfg.asrProfile(a.at(++i));
            } else if (k == QLatin1String("--hotwords") && i + 1 < a.size()) {
                hotwordsFile = a.at(++i);
            } else if (k == QLatin1String("--threads") && i + 1 < a.size()) {
                settings.asr.numThreads = a.at(++i).toInt();
            } else if (k == QLatin1String("--no-punct")) {
                settings.text.voicePunctuation    = false;
                settings.text.capitalizeSentences = false;
                settings.text.addFinalDot         = false;
            } else {
                std::fprintf(stderr, "неизвестная опция: %s\n", qPrintable(k));
                usage(a.value(0));
                return 2;
            }
        }
        std::printf("CFG : %s\n      профили: %s\n      активен: %s\n",
                    qPrintable(cfg.settingsPath()),
                    qPrintable(cfg.asrProfileNames().join(QStringLiteral(", "))),
                    qPrintable(settings.asr.name));
    } else {
        // --------- РЕЖИМ ЯВНЫХ ПУТЕЙ ---------
        if (a.size() < 6) {
            usage(a.value(0));
            return 2;
        }
        settings.vadModelPath = a.at(2);
        const QString engine  = a.at(3).toLower();

        settings.asr.name           = engine;
        settings.asr.engine         = AsrProfile::engineFromString(engine);
        settings.asr.numThreads     = 2;
        settings.asr.hotwordsScore  = 2.0f;

        if (settings.asr.engine == AsrProfile::Engine::Transducer) {
            if (a.size() < 8) { usage(a.value(0)); return 2; }
            settings.asr.encoderPath = a.at(4);
            settings.asr.decoderPath = a.at(5);
            settings.asr.joinerPath  = a.at(6);
            settings.asr.tokensPath  = a.at(7);
            if (a.size() > 8) hotwordsFile        = a.at(8);
            if (a.size() > 9) settings.asr.bpeVocabPath = a.at(9);
            if (!settings.asr.bpeVocabPath.isEmpty()) {
                settings.asr.modelingUnit = QStringLiteral("bpe");
            }
            settings.asr.decodingMethod = QStringLiteral("modified_beam_search");
        } else if (settings.asr.engine == AsrProfile::Engine::NemoCtc) {
            if (a.size() < 6) { usage(a.value(0)); return 2; }
            settings.asr.ctcModelPath = a.at(4);
            settings.asr.tokensPath   = a.at(5);
            settings.asr.decodingMethod = QStringLiteral("greedy_search");
        } else if (settings.asr.engine == AsrProfile::Engine::Whisper) {
            if (a.size() < 7) { usage(a.value(0)); return 2; }
            settings.asr.encoderPath = a.at(4);
            settings.asr.decoderPath = a.at(5);
            settings.asr.tokensPath  = a.at(6);
            if (a.size() > 7) settings.asr.language = a.at(7);
            settings.asr.decodingMethod = QStringLiteral("greedy_search");
        } else {
            std::fprintf(stderr, "unknown engine: %s\n", qPrintable(engine));
            usage(a.value(0));
            return 2;
        }
    }

    // --------- пайплайн ---------
    VoicePipeline pipeline(settings);

    QObject::connect(&pipeline, &VoicePipeline::errorOccurred, [](const QString& m) {
        std::fprintf(stderr, "[ERROR] %s\n", qPrintable(m));
    });
    QObject::connect(&pipeline, &VoicePipeline::speechStarted, []() {
        std::fprintf(stderr, "[VAD] speech started\n");
    });
    QObject::connect(&pipeline, &VoicePipeline::speechEnded, []() {
        std::fprintf(stderr, "[VAD] speech ended\n");
    });

    int segmentIndex = 0;
    double asrTotalMs = 0.0;
    QElapsedTimer segTimer;

    QObject::connect(&pipeline, &VoicePipeline::rawTextRecognized, [&](const QString& raw) {
        asrTotalMs += static_cast<double>(segTimer.elapsed());
        std::printf("  #%d  raw : %s\n", ++segmentIndex, qPrintable(raw));
        std::fflush(stdout);
    });
    QObject::connect(&pipeline, &VoicePipeline::textReady, [&](const QString& text) {
        std::printf("  #%d  text: %s\n", segmentIndex, qPrintable(text));
        std::fflush(stdout);
    });

    QString initError;
    segTimer.start();
    if (!pipeline.initialize(&initError)) {
        std::fprintf(stderr, "initialize() failed: %s\n", qPrintable(initError));
        return 1;
    }
    const double initMs = static_cast<double>(segTimer.elapsed());
    std::printf("Init: %.0f ms | ASR: %s (%s) | потоков: %d | decoding: %s\n",
                initMs,
                qPrintable(settings.asr.name),
                qPrintable(AsrProfile::engineToString(settings.asr.engine)),
                settings.asr.numThreads,
                qPrintable(settings.asr.decodingMethod));

    if (!hotwordsFile.isEmpty()) {
        const QStringList hw = readLines(hotwordsFile);
        pipeline.setHotwords(hw, settings.asr.hotwordsScore);
        std::printf("Hotwords: %d шт. из %s (score=%.1f)\n",
                    static_cast<int>(hw.size()), qPrintable(hotwordsFile),
                    settings.asr.hotwordsScore);
    }

    // Кормим окнами по 512 сэмплов (32 мс), как в примерах sherpa-onnx
    QElapsedTimer total;
    total.start();
    asrTotalMs = 0.0;
    segTimer.restart();

    const int windowBytes = 512 * static_cast<int>(sizeof(int16_t));
    const int totalBytes  = static_cast<int>(wav.mono.size() * sizeof(int16_t));
    const char* raw = reinterpret_cast<const char*>(wav.mono.data());

    for (int off = 0; off < totalBytes; off += windowBytes) {
        const int n = std::min(windowBytes, totalBytes - off);
        pipeline.processAudio(QByteArray(raw + off, n), kTargetRate);
    }
    pipeline.flush();

    std::printf("\nSegments: %d | audio %.2f s | wall %.0f ms | ASR %.0f ms | RTF %.2f\n",
                segmentIndex, wav.seconds, static_cast<double>(total.elapsed()), asrTotalMs,
                wav.seconds > 0.0 ? (asrTotalMs / 1000.0) / wav.seconds : 0.0);

    return segmentIndex > 0 ? 0 : 1;
}
