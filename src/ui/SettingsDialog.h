#pragma once

#include <QDialog>

class QSettings;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;
class QPushButton;
class QSpinBox;
class QGroupBox;
class QTableWidget;

// Диалог настроек — ЕДИНОЕ окно управления помощником: модель ASR, хоткей,
// вывод, голосовые подсказки (hotwords) и голосовые команды.
//
// Пишет напрямую в ~/.config/voice-assistant/settings.ini теми же ключами,
// которые читает ConfigManager, и в те же файлы, которые читают
// HotwordsManager (user_hotwords.txt) и CommandDictionary (commands.txt).
// Чтение — через ConfigManager (не дублируем его разбор, в частности граблю
// №9 со QStringList для значений с запятыми).
//
// После «Сохранить» испускает settingsApplied(); TrayIcon применяет изменения
// НА ЛЕТУ: switchAsrProfile(), setOutputTarget(), reloadHotkey(),
// reloadOutputSettings(), reloadHotwords(), reloadCommands().
//
// Команды двухслойные, и UI это не скрывает:
//   * встроенные — из loadDefaults() в коде: видны (фраза -> действие, с
//     фильтром), но не правятся; «скопировать в свои» переносит строку
//     «фраза = тип» в свой файл, где фразу можно заменить своей;
//   * свои — файл [commands] file: редактируются, при сохранении каждая
//     строка валидируется CommandDictionary::parseLine — той же функцией,
//     которой файл будет загружаться. Битые строки не сохраняются молча.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    enum Tab {
        TabAsr = 0,       // Распознавание
        TabControl,       // Управление (хоткей)
        TabOutput,        // Вывод
        TabHotwords,      // Подсказки (user_hotwords.txt)
        TabCommands       // Команды (встроенные + свои)
    };

    explicit SettingsDialog(QWidget* parent = nullptr, Tab initial = TabAsr);
    ~SettingsDialog();

signals:
    void settingsApplied();   // настройки записаны — применить на лету

private slots:
    void loadSettings();
    void saveSettings();
    void onKeyCapture();            // начать захват клавиши хоткея
    void onAsrProfileChanged(int);  // обновить статус выбранной модели
    void onPinWindowToggled(bool);  // показать/скрыть параметры привязки

    // Подсказки
    void addHotword();
    void removeHotword();
    void updateHotwordCount();

    // Команды
    void filterBuiltinCommands(const QString& text);
    void copyBuiltinToCustom();
    void addCustomCommand();
    void removeCustomCommand();

protected:
    // Захват клавиши: во время захвата QLineEdit стоит eventFilter, иначе
    // он съедал бы нажатия сам и до диалога они не доходили.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void finishKeyCapture(const QString& spec);   // клавиша распознана
    void cancelKeyCapture();
    QWidget* buildAsrTab();
    QWidget* buildControlTab();
    QWidget* buildOutputTab();
    QWidget* buildHotwordsTab();
    QWidget* buildCommandsTab();
    bool     saveHotwords();       // -> user_hotwords.txt, true = записано
    bool     saveCustomCommands(); // -> commands.txt с валидацией; false = есть битые строки

    QSettings* m_settings;

    // ASR
    QComboBox* m_asrProfileCombo;
    QLabel*    m_asrStatusLabel;    // движок, размер, готовность выбранного профиля

    // Hotkey
    QRadioButton* m_pttRadio;
    QRadioButton* m_toggleRadio;
    QLineEdit*    m_hotkeyEdit;
    QPushButton*  m_captureBtn;
    bool    m_isCapturingKey = false;
    QString m_hotkeyBeforeCapture;  // чтобы Esc отменял захват, а не стирал значение

    // Output
    QRadioButton* m_outputWindowRadio;
    QRadioButton* m_outputNotesRadio;
    QComboBox*    m_methodCombo;      // auto | clipboard | xdotool
    QCheckBox*    m_pinWindowCheck;
    QGroupBox*    m_pinGroup;         // параметры привязки (активен при pin_window)
    QComboBox*    m_pinModeCombo;     // activate | sendevent
    QSpinBox*     m_pinActivateSpin;  // pin_activate_ms
    QCheckBox*    m_pinRestoreCheck;  // pin_restore_focus

    // Подсказки (user_hotwords.txt)
    QListWidget* m_hotwordsList;
    QLineEdit*   m_hotwordInput;
    QLabel*      m_hotwordsCountLabel;
    QPushButton* m_hotwordRemoveBtn;
    QString      m_hotwordsPath;

    // Команды
    QTableWidget* m_builtinTable;     // read-only: фраза | действие (spec в UserRole)
    QLineEdit*    m_builtinFilter;
    QListWidget*  m_customList;       // строки «фраза = тип[:аргумент]», правка двойным кликом
    QPushButton*  m_copyBuiltinBtn;
    QPushButton*  m_customRemoveBtn;
    QCheckBox*    m_editInDictationCheck;   // [commands] editing_in_dictation
    QCheckBox*    m_notesVoiceCheck;        // [notes] voice_commands
    QString       m_commandsPath;

    QPushButton* m_saveBtn;
    QPushButton* m_cancelBtn;
};
