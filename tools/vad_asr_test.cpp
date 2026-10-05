// vad-asr-test — прогон аудиофайла через БОЕВОЙ путь: ConfigManager -> VoicePipeline
// (SileroVad -> IRecognizer -> TextPostProcessor), без UI и микрофона.
//
//   vad-asr-test <file> --config <settings.ini> [--profile <имя>] [--hotwords <file>]
//                       [--threads N] [--no-punct] [--notes] [--notes-file <путь>]
//                       [--rate N] [--channels N] [--format s16le|s8u|f32le|alaw|ulaw]
//
//   vad-asr-test <file> <silero_vad.onnx> transducer <enc> <dec> <joiner> <tokens>
//                       [hotwords-file] [bpe-vocab]
//   vad-asr-test <file> <silero_vad.onnx> nemo-ctc <model> <tokens>
//   vad-asr-test <file> <silero_vad.onnx> whisper <enc> <dec> <tokens> [language]
//
// Форматы — те же, что понимает приложение (общий AudioFileDecoder):
//   * WAV: PCM 8/16/24/32 бит, float32/64, A-law/μ-law, любые каналы и частота;
//   * MP3/OGG/FLAC/M4A/AMR… — через системный декодер (QtMultimedia/GStreamer);
//   * RAW (.raw/.pcm/без расширения) — параметры задают флаги --rate/--channels/
//     --format (по умолчанию телефонные 8000 Гц, моно, s16le).
// Всё домикшируется в моно и передискретизируется в 16 кГц.

#include "audio/AudioFileDecoder.h"
#include "config/ConfigManager.h"
#include "core/VoicePipeline.h"
#include "output/FileInjector.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kTargetRate = 16000;

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
        "  %s <file> --config <settings.ini> [--profile <name>] [--hotwords <file>]\n"
        "            [--threads N] [--no-punct] [--notes] [--notes-file <path>]\n"
        "            [--rate N] [--channels N] [--format s16le|s8u|f32le|alaw|ulaw]\n"
        "  %s <file> <silero_vad.onnx> transducer <enc> <dec> <joiner> <tokens>"
        " [hotwords-file] [bpe-vocab]\n"
        "  %s <file> <silero_vad.onnx> nemo-ctc <model> <tokens>\n"
        "  %s <file> <silero_vad.onnx> whisper <enc> <dec> <tokens> [language]\n",
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

    // Параметры RAW (PCM без контейнера) разбираем предварительным проходом:
    // в режиме --config флаги могут стоять где угодно, а positional-режим
    // их просто не содержит (проход на него не влияет).
    AudioFileDecoder::RawParams raw;
    for (int i = 2; i < a.size(); ++i) {
        if (a.at(i) == QLatin1String("--rate") && i + 1 < a.size()) {
            raw.sampleRate = a.at(++i).toInt();
        } else if (a.at(i) == QLatin1String("--channels") && i + 1 < a.size()) {
            raw.channels = a.at(++i).toInt();
        } else if (a.at(i) == QLatin1String("--format") && i + 1 < a.size()) {
            raw.format = AudioFileDecoder::RawParams::formatFromString(a.at(++i));
        }
    }

    AudioFileDecoder::Audio wav;
    QString wavError;
    if (!AudioFileDecoder::decode(wavPath, raw, wav, &wavError)) {
        std::fprintf(stderr, "Audio error: %s\n", qPrintable(wavError));
        return 1;
    }
    std::printf("FILE: %s\n      %d Hz, %d ch, %s -> %.2f s @ 16 kHz mono\n",
                qPrintable(wavPath), wav.sourceRate, wav.sourceChannels,
                qPrintable(wav.sourceFormat), wav.seconds());

    VoicePipeline::Settings settings;
    QString hotwordsFile;
    std::unique_ptr<ConfigManager> cfgPtr;   // нужен для --notes вне блока config
    bool   writeNotes = false;
    QString notesFileOverride;

    if (a.at(2) == QLatin1String("--config")) {
        // --------- БОЕВОЙ РЕЖИМ: всё из settings.ini ---------
        cfgPtr = std::make_unique<ConfigManager>(a.at(3));
        const ConfigManager& cfg = *cfgPtr;
        settings = VoicePipeline::loadSettings(cfg);

        for (int i = 4; i < a.size(); ++i) {
            const QString& k = a.at(i);
            if (k == QLatin1String("--profile") && i + 1 < a.size()) {
                settings.asr = cfg.asrProfile(a.at(++i));
            } else if (k == QLatin1String("--hotwords") && i + 1 < a.size()) {
                hotwordsFile = a.at(++i);
            } else if (k == QLatin1String("--threads") && i + 1 < a.size()) {
                settings.asr.numThreads = a.at(++i).toInt();
            } else if (k == QLatin1String("--notes")) {
                writeNotes = true;
            } else if (k == QLatin1String("--notes-file") && i + 1 < a.size()) {
                writeNotes       = true;
                notesFileOverride = a.at(++i);
            } else if (k == QLatin1String("--no-punct")) {
                settings.text.voicePunctuation    = false;
                settings.text.capitalizeSentences = false;
                settings.text.addFinalDot         = false;
            } else if (k == QLatin1String("--rate") || k == QLatin1String("--channels")
                       || k == QLatin1String("--format")) {
                ++i;   // учтены предварительным проходом выше
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

    // --------- заметки: тот же путь вывода, что и в приложении ---------
    // Нужен не только для теста: записанный микрофоном WAV (--record или
    // «Проверить микрофон») можно прогнать и сразу получить заметки в файле,
    // без микрофона и без X-сервера.
    std::unique_ptr<FileInjector> notesOut;
    if (writeNotes) {
        if (!cfgPtr) {
            std::fprintf(stderr, "--notes работает только в режиме --config\n");
            return 2;
        }
        FileInjector::Options nopt;
        nopt.dir            = cfgPtr->notesDir();
        nopt.file           = notesFileOverride.isEmpty() ? cfgPtr->notesFile()
                                                          : notesFileOverride;
        nopt.timestampFormat = cfgPtr->notesTimestampFormat();
        nopt.markdown       = cfgPtr->notesMarkdown();
        nopt.dayHeader      = cfgPtr->notesDayHeader();
        notesOut = std::make_unique<FileInjector>(nopt);
        if (!notesOut->initialize()) {
            std::fprintf(stderr, "--notes: %s\n", qPrintable(notesOut->lastError()));
            return 1;
        }
        std::printf("NOTES: %s\n", qPrintable(notesOut->filePath()));
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

    QObject::connect(&pipeline, &VoicePipeline::rawTextRecognized, [&](const QString& rawText) {
        asrTotalMs += static_cast<double>(segTimer.elapsed());
        std::printf("  #%d  raw : %s\n", ++segmentIndex, qPrintable(rawText));
        std::fflush(stdout);
    });
    QObject::connect(&pipeline, &VoicePipeline::textReady, [&](const QString& text) {
        std::printf("  #%d  text: %s\n", segmentIndex, qPrintable(text));
        if (notesOut && !notesOut->typeText(text)) {
            std::fprintf(stderr, "--notes: %s\n", qPrintable(notesOut->lastError()));
        }
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
    const int totalBytes  = static_cast<int>(wav.samples.size() * sizeof(int16_t));
    const char* rawPcm = reinterpret_cast<const char*>(wav.samples.data());

    for (int off = 0; off < totalBytes; off += windowBytes) {
        const int n = std::min(windowBytes, totalBytes - off);
        pipeline.processAudio(QByteArray(rawPcm + off, n), kTargetRate);
    }
    pipeline.flush();

    std::printf("\nSegments: %d | audio %.2f s | wall %.0f ms | ASR %.0f ms | RTF %.2f\n",
                segmentIndex, wav.seconds(), static_cast<double>(total.elapsed()), asrTotalMs,
                wav.seconds() > 0.0 ? (asrTotalMs / 1000.0) / wav.seconds() : 0.0);

    if (notesOut) {
        std::printf("Notes : %lld записей -> %s\n",
                    static_cast<long long>(notesOut->entries()),
                    qPrintable(notesOut->filePath()));
    }

    return segmentIndex > 0 ? 0 : 1;
}
