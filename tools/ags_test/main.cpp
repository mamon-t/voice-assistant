#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <QCoreApplication>
#include <QByteArray>
#include <QDebug>

#include "wav_reader.h"
#include "../../src/audio/Agc.h"

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " <wav_file> [options]\n"
              << "Options:\n"
              << "  --target-db <float>      Target level in dB (default: -15.0)\n"
              << "  --attack <float>         Attack time in seconds (default: 0.05)\n"
              << "  --release <float>        Release time in seconds (default: 0.2)\n"
              << "  --max-gain <float>       Maximum gain (default: 10.0)\n"
              << "  --min-gain <float>       Minimum gain (default: 0.01)\n"
              << "  --noise-gate <float>     Noise gate threshold in dB (default: -35.0)\n"
              << "  --chunk-size <int>       Chunk size in samples (default: 800)\n"
              << "  --output <file>          Output CSV file (default: stdout)\n";
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string wavFile = argv[1];
    
    // Параметры по умолчанию
    float targetDb = -15.0f;
    float attackTime = 0.05f;
    float releaseTime = 0.2f;
    float maxGain = 10.0f;
    float minGain = 0.01f;
    float noiseGate = -35.0f;
    int chunkSize = 800;  // 50 мс при 16 кГц
    std::string outputFile;
    
    // Парсим аргументы
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--target-db" && i + 1 < argc) {
            targetDb = std::stof(argv[++i]);
        } else if (arg == "--attack" && i + 1 < argc) {
            attackTime = std::stof(argv[++i]);
        } else if (arg == "--release" && i + 1 < argc) {
            releaseTime = std::stof(argv[++i]);
        } else if (arg == "--max-gain" && i + 1 < argc) {
            maxGain = std::stof(argv[++i]);
        } else if (arg == "--min-gain" && i + 1 < argc) {
            minGain = std::stof(argv[++i]);
        } else if (arg == "--noise-gate" && i + 1 < argc) {
            noiseGate = std::stof(argv[++i]);
        } else if (arg == "--chunk-size" && i + 1 < argc) {
            chunkSize = std::stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            outputFile = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // Читаем WAV
    WavData wav;
    try {
        wav = readWav(wavFile);
    } catch (const std::exception& e) {
        std::cerr << "Error reading WAV: " << e.what() << "\n";
        return 1;
    }
    
    std::cerr << "Loaded WAV: " << wav.samples.size() << " samples, "
              << wav.sampleRate << " Hz, " << wav.channels << " channels\n";
    
    // Настраиваем AGC
    Agc agc;
    agc.setTargetDb(targetDb);
    agc.setAttackTime(attackTime);
    agc.setReleaseTime(releaseTime);
    agc.setMaxGain(maxGain);
    agc.setMinGain(minGain);
    agc.setNoiseGateThresholdDb(noiseGate);
    
    // Открываем выходной файл
    std::ostream* out = &std::cout;
    std::ofstream outFile;
    if (!outputFile.empty()) {
        outFile.open(outputFile);
        if (!outFile) {
            std::cerr << "Cannot open output file: " << outputFile << "\n";
            return 1;
        }
        out = &outFile;
    }
    
    // Заголовок CSV
    *out << "chunk_num,input_rms_db,output_rms_db,gain,speech_detected\n";
    
    // Обрабатываем чанками
    int totalChunks = wav.samples.size() / chunkSize;
    
    // Общая статистика
    double sumInputDb = 0, sumOutputDb = 0, sumGain = 0;
    double sumOutputDbSquared = 0;
    double minInputDb = 0, maxInputDb = -200;
    double minOutputDb = 0, maxOutputDb = -200;
    int speechChunks = 0;
    
    // Статистика только для речи
    double speechInputDb = 0, speechOutputDb = 0, speechGain = 0;
    double speechOutputDbSquared = 0;
    double speechMinOutputDb = 0, speechMaxOutputDb = -200;
    
    for (int i = 0; i < totalChunks; ++i) {
        // Копируем чанк в QByteArray
        QByteArray chunkData(chunkSize * sizeof(int16_t), Qt::Uninitialized);
        int16_t* samples = reinterpret_cast<int16_t*>(chunkData.data());
        for (int j = 0; j < chunkSize; ++j) {
            samples[j] = wav.samples[i * chunkSize + j];
        }
        
        // Считаем входной RMS
        double inputSum = 0;
        for (int j = 0; j < chunkSize; ++j) {
            inputSum += samples[j] * samples[j];
        }
        double inputRms = std::sqrt(inputSum / chunkSize);
        double inputDb = (inputRms > 0) ? 20.0 * std::log10(inputRms / 32768.0) : -100.0;
        
        // Прогоняем через AGC
        QByteArray processedData = agc.process(chunkData, wav.sampleRate);
        
        // Считаем выходной RMS
        const int16_t* outSamples = reinterpret_cast<const int16_t*>(processedData.constData());
        double outputSum = 0;
        for (int j = 0; j < chunkSize; ++j) {
            outputSum += outSamples[j] * outSamples[j];
        }
        double outputRms = std::sqrt(outputSum / chunkSize);
        double outputDb = (outputRms > 0) ? 20.0 * std::log10(outputRms / 32768.0) : -100.0;
        
        float gain = agc.currentGain();
        bool speech = agc.isSpeechDetected();
        
        // Пишем в CSV
        *out << i << "," << inputDb << "," << outputDb << "," 
             << gain << "," << (speech ? 1 : 0) << "\n";
        
        // Собираем общую статистику
        sumInputDb += inputDb;
        sumOutputDb += outputDb;
        sumGain += gain;
        sumOutputDbSquared += outputDb * outputDb;
        
        if (i == 0 || inputDb < minInputDb) minInputDb = inputDb;
        if (inputDb > maxInputDb) maxInputDb = inputDb;
        if (i == 0 || outputDb < minOutputDb) minOutputDb = outputDb;
        if (outputDb > maxOutputDb) maxOutputDb = outputDb;
        
        // Собираем статистику только для речи
        if (speech) {
            speechChunks++;
            speechInputDb += inputDb;
            speechOutputDb += outputDb;
            speechGain += gain;
            speechOutputDbSquared += outputDb * outputDb;
            
            if (speechChunks == 1 || outputDb < speechMinOutputDb) {
                speechMinOutputDb = outputDb;
            }
            if (outputDb > speechMaxOutputDb) {
                speechMaxOutputDb = outputDb;
            }
        }
    }
    
    // Выводим статистику в stderr
    std::cerr << "\n=== Statistics ===\n"
              << "Total chunks: " << totalChunks << "\n"
              << "Speech chunks: " << speechChunks << " (" 
              << (100.0 * speechChunks / totalChunks) << "%)\n"
              << "\nInput RMS:\n"
              << "  Min: " << minInputDb << " dB\n"
              << "  Max: " << maxInputDb << " dB\n"
              << "  Avg: " << (sumInputDb / totalChunks) << " dB\n"
              << "\nOutput RMS:\n"
              << "  Min: " << minOutputDb << " dB\n"
              << "  Max: " << maxOutputDb << " dB\n"
              << "  Avg: " << (sumOutputDb / totalChunks) << " dB\n";
    
    // Считаем StdDev
    double avgOutputDb = sumOutputDb / totalChunks;
    double variance = (sumOutputDbSquared / totalChunks) - (avgOutputDb * avgOutputDb);
    double stddev = std::sqrt(std::max(0.0, variance));
    std::cerr << "  StdDev: " << stddev << " dB\n";
    
    std::cerr << "\nGain:\n"
              << "  Avg: " << (sumGain / totalChunks) << "\n";
    
    // Статистика только для речи
    if (speechChunks > 0) {
        double avgSpeechOutputDb = speechOutputDb / speechChunks;
        double speechVariance = (speechOutputDbSquared / speechChunks) - (avgSpeechOutputDb * avgSpeechOutputDb);
        double speechStddev = std::sqrt(std::max(0.0, speechVariance));
        
        std::cerr << "\n=== Speech-only Statistics ===\n"
                  << "Speech chunks: " << speechChunks << "\n"
                  << "Avg input RMS: " << (speechInputDb / speechChunks) << " dB\n"
                  << "Avg output RMS: " << avgSpeechOutputDb << " dB\n"
                  << "Output RMS range: [" << speechMinOutputDb << ", " << speechMaxOutputDb << "] dB\n"
                  << "Output RMS StdDev: " << speechStddev << " dB\n"
                  << "Avg gain: " << (speechGain / speechChunks) << "\n";
    }
    
    return 0;
}