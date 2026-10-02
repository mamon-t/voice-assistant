#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>

#include "core/Command.h"
#include "core/Mode.h"

class IAudioCapture;
class Agc;
class ConfigManager;
class VoicePipeline;
class ITextInjector;
class CommandParser;
class HotwordsManager;

// Сердце приложения: аудиопоток -> AGC -> VoicePipeline (VAD -> ASR -> пунктуация),
// затем маршрутизация результата: команда или вставка текста.
//
// Контроллер намеренно тонкий: всё, что связано с речью, живёт в VoicePipeline,
// выбор модели — в ConfigManager (профили), словарь команд — в CommandParser.
class ApplicationController : public QObject {
    Q_OBJECT

public:
    explicit ApplicationController(QObject* parent = nullptr);
    ~ApplicationController();

    Mode mode() const;
    void startRecording();
    void stopRecording();
    void setMode(Mode mode);

    // --- ASR-профили (переключение модели на лету) ---
    QStringList asrProfiles() const;
    QString     activeAsrProfile() const;
    bool        switchAsrProfile(const QString& profileName);

    // --- подсказки пользователя ---
    void reloadHotwords();

signals:
    void modeChanged(Mode mode);
    void errorOccurred(const QString& message);
    void textRecognized(const QString& text);        // что услышали (после постобработки)
    void commandExecuted(const QString& description); // для логов и UI

private slots:
    void onAudioDataReady(const QByteArray& data, int sampleRate);
    void onAudioError(const QString& message);
    void onRawTextRecognized(const QString& raw);    // до постобработки — ищем команду
    void onTextReady(const QString& text);           // после постобработки — вставляем

private:
    bool buildPipeline();
    void applyHotwords();
    void executeCommand(const Command& cmd);
    void injectText(const QString& text);

    Mode m_mode = Mode::Off;

    std::unique_ptr<ConfigManager>   m_config;
    std::unique_ptr<IAudioCapture>   m_audioCapture;
    std::unique_ptr<Agc>             m_agc;
    std::unique_ptr<VoicePipeline>   m_pipeline;
    std::unique_ptr<ITextInjector>   m_injector;
    std::unique_ptr<CommandParser>   m_commands;
    std::unique_ptr<HotwordsManager> m_hotwords;

    QByteArray m_audioBuffer;
    static constexpr int TARGET_CHUNK_SIZE = 1600; // 50 мс при 16 кГц

    bool m_skipNextText   = false;  // фраза оказалась командой — вставлять нечего
    bool m_audioDebugLog  = false;
    bool m_editCmdsInDictation = false;
};
