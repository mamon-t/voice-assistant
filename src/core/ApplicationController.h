#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

#include "audio/AudioFileDecoder.h"
#include "audio/TypingGuard.h"
#include "core/Command.h"
#include "core/Mode.h"
#include "core/OutputTarget.h"

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
class FileInjector;
class FileTranscriber;
class QThread;

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

    // --- словарь команд: пересобрать из кода + [commands] file ---
    // Нужно после правки своих команд во вкладке «Команды»: словарь и
    // командные hotwords перечитываются без перезапуска.
    void reloadCommands();

    // --- глобальный хоткей: перечитать [hotkey] из ini и перезапустить слушатель ---
    // Нужно после сохранения SettingsDialog: ключ/режим меняются без перезапуска.
    void reloadHotkey();

    // --- вывод текста: перечитать [output] из ini и перенастроить инжектор ---
    // method, задержки, pin_window/pin_mode/pin_activate_ms/pin_restore_focus,
    // own_window_class — всё подхватывается без перезапуска (нужно после
    // сохранения SettingsDialog). Уже установленная привязка к окну сохраняется.
    void reloadOutputSettings();

    // --- проверка микрофона: запись тракта в WAV ---
    // Пишет те же чанки, что уходят в ASR (по умолчанию после AGC — см.
    // [audio] mic_check_source), поэтому файл можно прогнать офлайн:
    //   ./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
    // Работает и в Mode::Off: захват запускается специально и режим не меняется.
    bool    startMicCheck(QString* outFile = nullptr);
    void    stopMicCheck();
    bool    isMicChecking() const;
    QString micCheckFile() const;

    // --- разбор аудиофайла («разбери файл» / пункт меню лотка) ---
    //
    // Сценарий: пользователь говорит команду (или жмёт пункт в лотке) ->
    // запоминаем окно, активное СЕЙЧАС (редактор), диктовку останавливаем ->
    // сигнал transcribeFileRequested -> UI показывает системный диалог выбора
    // файла -> startFileTranscription() -> рабочий поток (FileTranscriber)
    // декодирует файл и гонит его через отдельный VoicePipeline -> результат
    // вставляется в запомненное окно (цель «заметки» — в файл заметок).
    //
    // Окно запоминается ДО диалога выбора: диалог и прогресс — окна самого
    // помощника, и после их закрытия фокус непредсказуем. Доставка — тем же
    // механизмом pin (pin_mode=activate сам вернёт фокус).
    bool isTranscribing() const;
    void requestFileTranscription();
    bool startFileTranscription(const QString& filePath,
                                const QString& targetWindowId,
                                const AudioFileDecoder::RawParams& rawParams);
    void cancelFileTranscription();

    // --- глобальный хоткей ---
    bool    isHotkeyActive() const;
    QString hotkeyDescription() const;

    // --- цель вывода: активное окно или файл заметок ---
    //
    // Зачем: диктовка в окно конфликтует с ручной печатью. Результат приходит
    // через 0.5–2 с после фразы и уходит в то окно, которое в фокусе СЕЙЧАС,
    // а способ «буфер обмена + Ctrl+V» ещё и затирает буфер. Заметки в файл
    // не зависят ни от фокуса, ни от клавиатуры, ни от буфера: можно
    // диктовать пометки к файлу, с которым работаешь, продолжая печатать.
    OutputTarget outputTarget() const;
    bool        setOutputTarget(OutputTarget target);
    bool        toggleOutputTarget();
    QString     outputTargetName() const;      // "focus" | "notes" — для D-Bus
    QString     notesFilePath() const;         // куда пишутся заметки (пусто, если выключены)
    bool        isNotesAvailable() const;

    // К какому окну привязана вставка ([output] pin_window). Пустая строка —
    // привязки нет (выключена, окно не определилось или это было окно самого
    // помощника). Нужно для диагностики и для --pin-info.
    QString pinnedWindow() const { return m_pinnedWindow; }

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
    void outputTargetChanged(OutputTarget target);
    void noteWritten(const QString& path, const QString& text);

    // --- разбор аудиофайла ---
    // UI должен показать диалог выбора файла; targetWindowId — окно, куда
    // вставлять результат (пусто, если активное окно не определилось или
    // это окно самого помощника — тогда вставка пойдёт в текущий фокус).
    void transcribeFileRequested(const QString& targetWindowId);
    void fileTranscriptionStarted(const QString& filePath);
    void fileTranscriptionProgress(int percent, const QString& stage);
    void fileTranscriptionFinished(const QString& filePath, const QString& text, int segments);
    void fileTranscriptionFailed(const QString& filePath, const QString& error);
    void fileTranscriptionCancelled(const QString& filePath);

private slots:
    void onAudioDataReady(const QByteArray& data, int sampleRate);
    void onAudioError(const QString& message);
    void onRawTextRecognized(const QString& raw);    // до постобработки — ищем команду
    void onTextReady(const QString& text);           // после постобработки — вставляем
    void onKeyActivity();                            // пользователь печатает — см. TypingGuard
    void onFileTranscriptionSucceeded(const QString& text, int segments,
                                      double audioSeconds, double wallMs);
    void onFileTranscriptionFailed(const QString& error);
    void onFileTranscriptionCancelled();

private:
    bool buildPipeline();
    void applyHotwords();
    void executeCommand(const Command& cmd);
    void injectText(const QString& text);
    // Инжектор текущей цели вывода: файл заметок или окно. nullptr, если
    // соответствующий вывод недоступен.
    ITextInjector* activeInjector() const;
    void runSpellcheck(const QString& text);
    void setupHotkey();
    void onHotkeyPressed();
    void onHotkeyReleased();
    void setupNotes();
    // [output] pin_window: запомнить окно, активное в НАЧАЛЕ записи, и вставлять
    // в него, даже если фокус ушёл в другой файл. Окна самого помощника (меню
    // трея, настройки) привязкой НЕ становятся: вставка в них теряет текст.
    void captureTargetWindow();
    // Активное окно принадлежит самому помощнику (WM_CLASS из
    // [output] own_window_class или PID нашего процесса) — см. captureTargetWindow.
    bool isOwnWindow(const QString& windowId) const;
    // Активное окно, если оно НЕ наше; иначе пустая строка.
    QString currentForeignWindowId() const;
    // Остановить рабочий поток разбора файла и освободить объекты.
    void cleanupTranscriber();

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
    std::unique_ptr<FileInjector>    m_notes;

    // --- разбор аудиофайла: рабочий поток и его состояние ---
    QThread*        m_transcribeThread = nullptr;
    FileTranscriber* m_transcriber     = nullptr;   // живёт в m_transcribeThread
    QString         m_transcribeFile;               // какой файл разбираем
    QString         m_transcribeTargetWindow;       // куда вставлять результат

    bool    m_micCheckOwnsCapture = false;   // захват запущен специально для записи
    QString m_micCheckSource;                // "agc" | "raw"
    QString m_hotkeyMode;                    // "push_to_talk" | "toggle"

    OutputTarget  m_target = OutputTarget::Focus;
    TypingGuard   m_typingGuard;             // глушим микрофон, пока идёт печать
    QElapsedTimer m_clock;                   // единые часы для TypingGuard
    bool          m_pinWindow = false;       // [output] pin_window
    QString       m_pinnedWindow;            // WID окна, в которое вставляем

    QByteArray m_audioBuffer;
    static constexpr int TARGET_CHUNK_SIZE = 1600; // 50 мс при 16 кГц

    void reportError(const QString& message);

    QString m_lastError;
    QString m_lastInjected;      // чтобы ставить пробел между сегментами
    bool m_skipNextText   = false;  // фраза оказалась командой — вставлять нечего
    bool m_audioDebugLog  = false;
    bool m_editCmdsInDictation = false;
};
