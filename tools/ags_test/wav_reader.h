#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <stdexcept>

struct WavData {
    std::vector<int16_t> samples;
    int sampleRate = 0;
    int channels = 0;
};

inline WavData readWav(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + filePath);
    }

    // Читаем RIFF заголовок
    char riff[4];
    file.read(riff, 4);
    if (std::string(riff, 4) != "RIFF") {
        throw std::runtime_error("Not a RIFF file");
    }

    uint32_t fileSize;
    file.read(reinterpret_cast<char*>(&fileSize), 4);

    char wave[4];
    file.read(wave, 4);
    if (std::string(wave, 4) != "WAVE") {
        throw std::runtime_error("Not a WAVE file");
    }

    // Ищем fmt chunk
    char chunkId[4];
    uint32_t chunkSize;
    
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    
    while (file.read(chunkId, 4)) {
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        
        if (std::string(chunkId, 4) == "fmt ") {
            uint16_t audioFormat;
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            if (audioFormat != 1) {  // 1 = PCM
                throw std::runtime_error("Not PCM format");
            }
            
            file.read(reinterpret_cast<char*>(&channels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            
            uint32_t byteRate;
            file.read(reinterpret_cast<char*>(&byteRate), 4);
            
            uint16_t blockAlign;
            file.read(reinterpret_cast<char*>(&blockAlign), 2);
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            
            // Пропускаем остаток fmt chunk, если есть
            if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
        } 
        else if (std::string(chunkId, 4) == "data") {
            // Читаем аудио данные
            std::vector<uint8_t> rawData(chunkSize);
            file.read(reinterpret_cast<char*>(rawData.data()), chunkSize);
            
            if (bitsPerSample != 16) {
                throw std::runtime_error("Only 16-bit samples supported");
            }
            
            int numSamples = chunkSize / 2;
            WavData result;
            result.sampleRate = sampleRate;
            result.channels = channels;
            result.samples.resize(numSamples);
            
            for (int i = 0; i < numSamples; ++i) {
                result.samples[i] = static_cast<int16_t>(
                    rawData[i * 2] | (rawData[i * 2 + 1] << 8)
                );
            }
            
            return result;
        }
        else {
            // Пропускаем неизвестный chunk
            file.seekg(chunkSize, std::ios::cur);
        }
    }
    
    throw std::runtime_error("No data chunk found");
}