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
    float punctuationHotwordsScore() const;   // [asr] punctuation_hotwords_score
    float commandsHotwordsScore() const;      // [asr] commands_hotwords_score

    // --- постобработка текста ---
    bool autoPunctuate() const;
    bool voicePunctuation() const;
    bool capitalizeSentences() const;   // [text] capitalize
    bool addFinalDot() const;           // [text] add_final_dot

    // --- вывод текста (ITextInjector) ---
    QString injectorMethod() const;    // [output] method: auto | xdotool | clipboard
    int     typingDelayMs() const;     // [output] typing_delay_ms
    bool    preserveClipboard() const; // [output] preserve_clipboard
    bool    spaceBetweenSegments() const; // [output] space_between_segments
    int     clipboardRestoreMs() const;// [output] clipboard_restore_ms
    // Привязывать вставку к окну, которое было активным в НАЧАЛЕ записи, а не
    // к тому, что в фокусе сейчас. Нужно, чтобы можно было переключиться
    // в другой файл и печатать там, пока диктовка продолжается.
    bool    pinWindow() const;           // [output] pin_window

    // КАК доставлять текст в привязанное окно — см. output/WindowTarget.h.
    //   activate  (по умолчанию) — окно активируется, текст печатается
    //             настоящими событиями, фокус возвращается. Работает везде.
    //   sendevent — прежнее поведение (xdotool --window, XSendEvent): фокус не
    //             трогается, зато браузеры/LibreOffice/Java такой ввод часто
    //             отбрасывают, и xdotool всё равно возвращает 0.
    QString pinMode() const;             // [output] pin_mode
    int     pinActivateMs() const;       // [output] pin_activate_ms
    bool    pinRestoreFocus() const;     // [output] pin_restore_focus
    // WM_CLASS окон самого помощника: привязываться к ним нельзя, иначе текст
    // уходит в меню трея и пропадает вместе с ним. Список через запятую.
    QStringList ownWindowClasses() const;// [output] own_window_class

    // --- заметки: вторая цель вывода (OutputTarget::Notes) ---
    bool    notesEnabled() const;          // [notes] enabled
    QString notesDir() const;              // [notes] dir   (один файл на день)
    QString notesFile() const;             // [notes] file  (явный файл вместо ежедневного)
    QString notesTimestampFormat() const;  // [notes] timestamp ("HH:mm:ss", пусто = без метки)
    bool    notesMarkdown() const;         // [notes] markdown
    bool    notesDayHeader() const;        // [notes] day_header
    bool    notesVoiceCommands() const;    // [notes] voice_commands («заметка»/«в редактор»)
    QString notesStartTarget() const;      // [notes] start_target: focus | notes

    // --- команды ---
    bool    editingCommandsInDictation() const;  // [commands] editing_in_dictation
    QString customCommandsPath() const;          // [commands] file

    // --- проверка правописания ---
    bool    spellcheckEnabled() const;        // [spellcheck] enabled
    QString spellcheckLang() const;           // [spellcheck] lang (ru_RU)
    QString spellcheckDictionaryDir() const;  // [spellcheck] dictionary_dir
    int     spellcheckMaxSuggestions() const; // [spellcheck] max_suggestions

    // --- проверка микрофона (запись тракта в WAV) ---
    QString micCheckDir() const;        // [audio] mic_check_dir
    QString micCheckSource() const;     // [audio] mic_check_source: agc | raw
    QString micCheckPrefix() const;     // [audio] mic_check_prefix

    // --- глобальный хоткей микрофона ---
    QString hotkeyBackend() const;      // [hotkey] backend: evdev | off
    QString hotkeyKey() const;          // [hotkey] key: KEY_F8, ctrl+space, ...
    bool    hotkeyGrab() const;         // [hotkey] grab: перехватывать клавишу (EVIOCGRAB)
    // Список устройств, которые разрешено перехватывать эксклюзивно (подстрока
    // имени или пути: "footswitch", "/dev/input/event7"). Пусто = grab
    // применяется только к устройствам, не похожим на полноценную клавиатуру.
    // Это защита от того, чтобы основная клавиатура ушла из-под X.
    QStringList hotkeyGrabDevices() const;   // [hotkey] grab_devices
    QString hotkeyMode() const;         // [hotkey] mode: push_to_talk | toggle

    // --- отладка ---
    bool audioDebugLog() const;        // [audio] debug_log

    // Глушить микрофон, пока идёт печать (см. audio/TypingGuard.h).
    // 0 = выключено. Работает только при [hotkey] backend=evdev.
    int  typingGuardMs() const;        // [audio] typing_guard_ms

private:
    QSettings m_settings;
};
