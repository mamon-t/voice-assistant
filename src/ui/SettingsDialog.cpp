#include "SettingsDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QKeySequence>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
    , m_isCapturingKey(false)
{
    setWindowTitle("Настройки голосового помощника");
    resize(400, 300);

    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/voice-assistant/settings.ini";
    m_settings = new QSettings(configPath, QSettings::IniFormat, this);

    // --- Вкладки ---
    QTabWidget* tabWidget = new QTabWidget(this);

    // 1. Вкладка Распознавание
    QWidget* asrTab = new QWidget();
    QFormLayout* asrLayout = new QFormLayout(asrTab);
    
    m_asrProfileCombo = new QComboBox();
    // Заполним дефолтными значениями, позже можно читать из конфига динамически
    m_asrProfileCombo->addItems({"zipformer-ru", "gigaam-v3", "whisper-base"});
    asrLayout->addRow("Модель ASR:", m_asrProfileCombo);
    asrLayout->addItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

    // 2. Вкладка Управление
    QWidget* hotkeyTab = new QWidget();
    QVBoxLayout* hotkeyLayout = new QVBoxLayout(hotkeyTab);
    
    QGroupBox* modeGroup = new QGroupBox("Режим активации");
    QVBoxLayout* modeLayout = new QVBoxLayout(modeGroup);
    m_pttRadio = new QRadioButton("Push-to-talk (удерживать)");
    m_toggleRadio = new QRadioButton("Toggle (вкл/выкл по нажатию)");
    modeLayout->addWidget(m_pttRadio);
    modeLayout->addWidget(m_toggleRadio);
    hotkeyLayout->addWidget(modeGroup);

    QHBoxLayout* keyLayout = new QHBoxLayout();
    m_hotkeyEdit = new QLineEdit();
    m_hotkeyEdit->setReadOnly(true);
    m_hotkeyEdit->setPlaceholderText("Нажмите для захвата клавиши...");
    m_captureBtn = new QPushButton("Захватить");
    connect(m_captureBtn, &QPushButton::clicked, this, &SettingsDialog::onKeyCapture);
    keyLayout->addWidget(new QLabel("Глобальная клавиша:"));
    keyLayout->addWidget(m_hotkeyEdit);
    keyLayout->addWidget(m_captureBtn);
    hotkeyLayout->addLayout(keyLayout);
    hotkeyLayout->addItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

    // 3. Вкладка Вывод
    QWidget* outputTab = new QWidget();
    QVBoxLayout* outputLayout = new QVBoxLayout(outputTab);
    
    QGroupBox* targetGroup = new QGroupBox("Цель вывода по умолчанию");
    QVBoxLayout* targetLayout = new QVBoxLayout(targetGroup);
    m_outputWindowRadio = new QRadioButton("В активное окно (через буфер/xdotool)");
    m_outputNotesRadio = new QRadioButton("В файл заметок (~/.local/share/voice-assistant/notes/)");
    targetLayout->addWidget(m_outputWindowRadio);
    targetLayout->addWidget(m_outputNotesRadio);
    outputLayout->addWidget(targetGroup);
    outputLayout->addItem(new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding));

    tabWidget->addTab(asrTab, "Распознавание");
    tabWidget->addTab(hotkeyTab, "Управление");
    tabWidget->addTab(outputTab, "Вывод");

    // --- Кнопки ---
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveBtn = new QPushButton("Сохранить");
    m_cancelBtn = new QPushButton("Отмена");
    connect(m_saveBtn, &QPushButton::clicked, this, &SettingsDialog::saveSettings);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);

    // --- Основной лейаут ---
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(tabWidget);
    mainLayout->addLayout(btnLayout);

    loadSettings();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::loadSettings() {
    m_asrProfileCombo->setCurrentText(m_settings->value("asr/active", "zipformer-ru").toString());
    
    QString hkType = m_settings->value("hotkey/type", "ptt").toString();
    m_pttRadio->setChecked(hkType == "ptt");
    m_toggleRadio->setChecked(hkType == "toggle");
    
    QString hkKey = m_settings->value("hotkey/key", "F8").toString();
    m_hotkeyEdit->setText(hkKey);

    QString target = m_settings->value("output/target", "window").toString();
    m_outputWindowRadio->setChecked(target == "window");
    m_outputNotesRadio->setChecked(target == "notes");
}

void SettingsDialog::saveSettings() {
    m_settings->setValue("asr/active", m_asrProfileCombo->currentText());
    m_settings->setValue("hotkey/type", m_pttRadio->isChecked() ? "ptt" : "toggle");
    m_settings->setValue("hotkey/key", m_hotkeyEdit->text());
    m_settings->setValue("output/target", m_outputWindowRadio->isChecked() ? "window" : "notes");
    
    m_settings->sync();
    emit settingsApplied();
    accept();
}

void SettingsDialog::onKeyCapture() {
    m_isCapturingKey = true;
    m_hotkeyEdit->setText("Нажмите клавишу...");
    m_hotkeyEdit->setFocus();
}

void SettingsDialog::keyPressEvent(QKeyEvent* event) {
    if (m_isCapturingKey) {
        if (event->key() == Qt::Key_Escape) {
            m_isCapturingKey = false;
            loadSettings(); // Вернуть старое значение
            return;
        }
        
        QKeySequence keySeq(event->key());
        m_hotkeyEdit->setText(keySeq.toString());
        m_isCapturingKey = false;
        return;
    }
    
    QDialog::keyPressEvent(event);
}