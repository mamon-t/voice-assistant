#pragma once

#include <QDialog>

class QSettings;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QPushButton;

// Диалог настроек: модель ASR, хоткей микрофона, цель вывода при старте.
//
// Пишет напрямую в ~/.config/voice-assistant/settings.ini теми же ключами,
// которые читает ConfigManager (asr/active, hotkey/mode, hotkey/key,
// notes/start_target). Читает — через ConfigManager, чтобы не дублировать
// его разбор (в частности, граблю №9 со QStringList для значений с запятыми).
//
// После «Сохранить» испускает settingsApplied(); TrayIcon применяет изменения
// на лету: switchAsrProfile(), setOutputTarget(), reloadHotkey().
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
    void onKeyCapture();      // начать захват клавиши хоткея

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

    // Hotkey
    QRadioButton* m_pttRadio;
    QRadioButton* m_toggleRadio;
    QLineEdit* m_hotkeyEdit;
    QPushButton* m_captureBtn;
    bool    m_isCapturingKey;
    QString m_hotkeyBeforeCapture;   // чтобы Esc отменял захват, а не стирал значение

    // Output
    QRadioButton* m_outputWindowRadio;
    QRadioButton* m_outputNotesRadio;

    QPushButton* m_saveBtn;
    QPushButton* m_cancelBtn;
};
