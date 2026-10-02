#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

#include "config/AsrProfile.h"

// Чтение/запись ~/.config/voice-assistant/settings.ini.
//
// ИЗМЕНЕНИЯ относительно прошлой версии:
//   + resolvePath()          — "~/…", относительные пути от modelsPath(), абсолютные
//   + asrProfile()/activeAsrProfile()/asrProfileNames() — профили моделей, секции [asr_<имя>]
//   + vadMinSilenceDuration(), vadMinSpeechDuration(), vadWindowSize(),
//     vadNumThreads(), vadBufferSeconds()
//   + autoPunctuate(), voicePunctuation()
//   + конструктор с явным путём к .ini (нужен для тестов и для --config)
//   - senseVoiceModelPath()/senseVoiceTokensPath() — SenseVoice удалён (не знает русского)
class ConfigManager {
public:
    ConfigManager();
    explicit ConfigManager(const QString& iniPath);

    // --- пути ---
    QString configDir() const;
    QString settingsPath() const;
    QString modelsPath() const;

    // "~" раскрывается в home, относительный путь считается от modelsPath(),
    // абсолютный возвращается как есть.
    QString resolvePath(const QString& path) const;

    QString sileroVadPath() const;
    QString commandsHotwordsPath() const;
    QString userHotwordsPath() const;

    QStringList loadHotwords(const QString& filePath) const;
    void saveHotwords(const QString& filePath, const QStringList& words) const;

    // --- VAD ---
    float vadThreshold() const;
    void  setVadThreshold(float threshold);
    float vadMinSilenceDuration() const;
    float vadMinSpeechDuration() const;
    int   vadWindowSize() const;
    int   vadNumThreads() const;
    float vadBufferSeconds() const;

    // --- ASR-профили ---
    QStringList asrProfileNames() const;
    QString     activeAsrProfileName() const;
    void        setActiveAsrProfileName(const QString& name);
    bool        hasAsrProfile(const QString& name) const;
    AsrProfile  asrProfile(const QString& name) const;
    AsrProfile  activeAsrProfile() const;

    int   asrNumThreads() const;                 // дефолт, если в профиле не задан
    void  setAsrNumThreads(int threads);
    float hotwordsScore() const;
    void  setHotwordsScore(float score);

    // --- постобработка текста ---
    bool autoPunctuate() const;
    bool voicePunctuation() const;

private:
    QSettings m_settings;
};
