#pragma once

#include <QDialog>

class QSettings;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QPushButton;
class QSpinBox;
class QGroupBox;

// Диалог настроек: модель ASR, хоткей микрофона, вывод текста.
//
// Пишет напрямую в ~/.config/voice-assistant/settings.ini теми же ключами,
// которые читает ConfigManager (asr/active, hotkey/mode, hotkey/key,
// notes/start_target, output/method, output/pin_window, output/pin_mode,
// output/pin_activate_ms, output/pin_restore_focus). Читает — через
// ConfigManager, чтобы не дублировать его разбор (в частности, граблю №9
// со QStringList для значений с запятыми).
//
// После «Сохранить» испускает settingsApplied(); TrayIcon применяет изменения
// НА ЛЕТУ: switchAsrProfile(), setOutputTarget(), reloadHotkey(),
// reloadOutputSettings() — перезапуск приложения не нужен.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog();

signals:
    void settingsApplied();   // настройки записаны в ini — применить на лету

private slots:
    void loadSettings();
    void saveSettings();
    void onKeyCapture();            // начать захват клавиши хоткея
    void onAsrProfileChanged(int);  // обновить статус выбранной модели
    void onPinWindowToggled(bool);  // показать/скрыть параметры привязки

protected:
    // Захват клавиши: во время захвата QLineEdit стоит eventFilter, иначе
    // он съедал бы нажатия сам и до диалога они не доходили.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void finishKeyCapture(const QString& spec);   // клавиша распознана
    void cancelKeyCapture();

    QSettings* m_settings;

    // ASR
    QComboBox* m_asrProfileCombo;
    QLabel*    m_asrStatusLabel;    // движок, размер, готовность выбранного профиля

    // Hotkey
    QRadioButton* m_pttRadio;
    QRadioButton* m_toggleRadio;
    QLineEdit*    m_hotkeyEdit;
    QPushButton*  m_captureBtn;
    bool    m_isCapturingKey;
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

    QPushButton* m_saveBtn;
    QPushButton* m_cancelBtn;
};
