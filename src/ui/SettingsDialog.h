#pragma once

#include <QDialog>
#include <QSettings>
#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>
#include <QPushButton>

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog();

signals:
    void settingsApplied(); // Сигнал для ApplicationController, чтобы перезагрузить модель/хоткей

private slots:
    void loadSettings();
    void saveSettings();
    void onKeyCapture(); // Для захвата нажатия клавиши

private:
    QSettings* m_settings;
    
    // ASR
    QComboBox* m_asrProfileCombo;
    
    // Hotkey
    QRadioButton* m_pttRadio;
    QRadioButton* m_toggleRadio;
    QLineEdit* m_hotkeyEdit;
    QPushButton* m_captureBtn;
    bool m_isCapturingKey;

    // Output
    QRadioButton* m_outputWindowRadio;
    QRadioButton* m_outputNotesRadio;

    QPushButton* m_saveBtn;
    QPushButton* m_cancelBtn;

    void keyPressEvent(QKeyEvent* event) override;
};