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
class ISpellChecker;
class IHotkeyListener;
class WavWriter;

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

    // --- проверка микрофона: запись тракта в WAV ---
    // Пишет те же чанки, что уходят в ASR (по умолчанию после AGC — см.
    // [audio] mic_check_source), поэтому файл можно прогнать офлайн:
    //   ./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
    // Работает и в Mode::Off: захват запускается специально и режим не меняется.
    bool    startMicCheck(QString* outFile = nullptr);
    void    stopMicCheck();
    bool    isMicChecking() const;
    QString micCheckFile() const;

    // --- глобальный хоткей ---
    bool    isHotkeyActive() const;
    QString hotkeyDescription() const;

    // Последняя ошибка, в том числе возникшая в конструкторе (тогда её ещё
    // некому было принять сигналом). Пустая строка — ошибок не было.
    QString lastError() const { return m_lastError; }

    // --- проверка правописания ---
    bool      isSpellcheckAvailable() const;
    QStringList checkText(const QString& text);        // слова с ошибками
    QStringList suggestionsFor(const QString& word);   // варианты исправления
    QString   spellcheckStatus() const;                // одна строка для логов и --check

    // Готов ли тракт к работе (VAD и ASR созданы).
    bool isReady() const;

signals:
    void modeChanged(Mode mode);
    void errorOccurred(const QString& message);
    void textRecognized(const QString& text);        // что услышали (после постобработки)
    void commandExecuted(const QString& description); // для логов и UI
    void spellcheckFinished(const QString& text, const QStringList& errors);
    void micCheckStarted(const QString& path);
    void micCheckFinished(const QString& path, double seconds);

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
    void runSpellcheck(const QString& text);
    void setupHotkey();
    void onHotkeyPressed();
    void onHotkeyReleased();

    Mode m_mode = Mode::Off;

    std::unique_ptr<ConfigManager>   m_config;
    std::unique_ptr<IAudioCapture>   m_audioCapture;
    std::unique_ptr<Agc>             m_agc;
    std::unique_ptr<VoicePipeline>   m_pipeline;
    std::unique_ptr<ITextInjector>   m_injector;
    std::unique_ptr<CommandParser>   m_commands;
    std::unique_ptr<HotwordsManager> m_hotwords;
    std::unique_ptr<ISpellChecker>   m_spellChecker;
    std::unique_ptr<IHotkeyListener> m_hotkey;
    std::unique_ptr<WavWriter>       m_wavWriter;

    bool    m_micCheckOwnsCapture = false;   // захват запущен специально для записи
    QString m_micCheckSource;                // "agc" | "raw"
    QString m_hotkeyMode;                    // "push_to_talk" | "toggle"

    QByteArray m_audioBuffer;
    static constexpr int TARGET_CHUNK_SIZE = 1600; // 50 мс при 16 кГц

    void reportError(const QString& message);

    QString m_lastError;
    QString m_lastInjected;      // чтобы ставить пробел между сегментами
    bool m_skipNextText   = false;  // фраза оказалась командой — вставлять нечего
    bool m_audioDebugLog  = false;
    bool m_editCmdsInDictation = false;
};
