#include "SettingsDialog.h"

#include "config/ConfigManager.h"
#include "input/EvdevHotkeyListener.h"

#include <QComboBox>
#include <QDebug>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

// Qt::Key -> имя, которое понимает EvdevHotkeyListener::parseKey()
// (см. kKeyNames: f1..f16, f20, space, tab, return, стрелки и т.д.).
// Буквы и цифры НАМЕРЕННО не поддерживаются: хоткей микрофона не должен
// мешать печатать, поэтому годятся только клавиши, которыми никто не
// пользуется (F13–F24, Scroll Lock, Pause, Mic Mute) или комбинация.
QString qtKeyToEvdevName(int key)
{
    if (key >= Qt::Key_F1 && key <= Qt::Key_F16) {
        return QStringLiteral("KEY_F%1").arg(key - Qt::Key_F1 + 1);
    }
    switch (key) {
    case Qt::Key_F20:        return QStringLiteral("KEY_F20");
    case Qt::Key_Space:      return QStringLiteral("space");
    case Qt::Key_Tab:        return QStringLiteral("tab");
    case Qt::Key_Enter:
    case Qt::Key_Return:     return QStringLiteral("return");
    case Qt::Key_Backspace:  return QStringLiteral("backspace");
    case Qt::Key_Insert:     return QStringLiteral("insert");
    case Qt::Key_Delete:     return QStringLiteral("delete");
    case Qt::Key_Home:       return QStringLiteral("home");
    case Qt::Key_End:        return QStringLiteral("end");
    case Qt::Key_PageUp:     return QStringLiteral("pageup");
    case Qt::Key_PageDown:   return QStringLiteral("pagedown");
    case Qt::Key_Up:         return QStringLiteral("up");
    case Qt::Key_Down:       return QStringLiteral("down");
    case Qt::Key_Left:       return QStringLiteral("left");
    case Qt::Key_Right:      return QStringLiteral("right");
    case Qt::Key_ScrollLock: return QStringLiteral("scrolllock");
    case Qt::Key_Pause:      return QStringLiteral("pause");
    default:                 return QString();
    }
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
    , m_isCapturingKey(false)
{
    setWindowTitle(QStringLiteral("Настройки голосового помощника"));
    resize(460, 340);

    // Тот же файл, что читает ConfigManager. Запись — через QSettings,
    // чтение — через ConfigManager (он знает про грабли разбора ini).
    const ConfigManager cfg;
    m_settings = new QSettings(cfg.settingsPath(), QSettings::IniFormat, this);

    auto* tabWidget = new QTabWidget(this);

    // --- 1. Распознавание ---------------------------------------------------
    auto* asrTab = new QWidget();
    auto* asrLayout = new QFormLayout(asrTab);

    m_asrProfileCombo = new QComboBox();
    // Профили — из [asr] profiles, без хардкода: список должен совпадать
    // с тем, что реально настроено в ini (и с --check).
    m_asrProfileCombo->addItems(cfg.asrProfileNames());
    asrLayout->addRow(QStringLiteral("Модель ASR:"), m_asrProfileCombo);
    asrLayout->addRow(new QLabel(
        QStringLiteral("Переключение применяется сразу: модель перезагружается "
                       "за 1–2 с (в это время диктовка недоступна).")));

    // --- 2. Управление ------------------------------------------------------
    auto* hotkeyTab = new QWidget();
    auto* hotkeyLayout = new QVBoxLayout(hotkeyTab);

    auto* modeGroup = new QGroupBox(QStringLiteral("Режим активации"));
    auto* modeLayout = new QVBoxLayout(modeGroup);
    m_pttRadio    = new QRadioButton(QStringLiteral("Push-to-talk (держать клавишу, пока говорите)"));
    m_toggleRadio = new QRadioButton(QStringLiteral("Toggle (нажал — запись, отжал — остановка)"));
    modeLayout->addWidget(m_pttRadio);
    modeLayout->addWidget(m_toggleRadio);
    hotkeyLayout->addWidget(modeGroup);

    auto* keyLayout = new QHBoxLayout();
    m_hotkeyEdit = new QLineEdit();
    m_hotkeyEdit->setReadOnly(true);
    m_hotkeyEdit->setPlaceholderText(QStringLiteral("Нажмите «Захватить», затем клавишу..."));
    m_captureBtn = new QPushButton(QStringLiteral("Захватить"));
    connect(m_captureBtn, &QPushButton::clicked, this, &SettingsDialog::onKeyCapture);
    keyLayout->addWidget(new QLabel(QStringLiteral("Глобальная клавиша:")));
    keyLayout->addWidget(m_hotkeyEdit);
    keyLayout->addWidget(m_captureBtn);
    hotkeyLayout->addLayout(keyLayout);

    hotkeyLayout->addWidget(new QLabel(
        QStringLiteral("Поддерживаются: F1–F16, F20, пробел, Tab, Enter, стрелки,\n"
                       "Scroll Lock, Pause и комбинации с ctrl/alt/shift/super.\n"
                       "Буквы и цифры не годятся: хоткей не должен мешать печатать.\n"
                       "Нужны права на /dev/input: sudo usermod -aG input $USER.")));
    hotkeyLayout->addStretch();

    // --- 3. Вывод -------------------------------------------------------------
    auto* outputTab = new QWidget();
    auto* outputLayout = new QVBoxLayout(outputTab);

    auto* targetGroup = new QGroupBox(QStringLiteral("Цель вывода при старте"));
    auto* targetLayout = new QVBoxLayout(targetGroup);
    m_outputWindowRadio = new QRadioButton(
        QStringLiteral("Активное окно (буфер обмена + Ctrl+V или xdotool)"));
    m_outputNotesRadio = new QRadioButton(
        QStringLiteral("Файл заметок (~/.local/share/voice-assistant/notes/)"));
    targetLayout->addWidget(m_outputWindowRadio);
    targetLayout->addWidget(m_outputNotesRadio);
    outputLayout->addWidget(targetGroup);
    outputLayout->addWidget(new QLabel(
        QStringLiteral("Переключать цель на лету можно голосом («заметка» / «в редактор»)\n"
                       "или из меню лотка; здесь задаётся только стартовое значение\n"
                       "([notes] start_target).")));
    outputLayout->addStretch();

    tabWidget->addTab(asrTab,     QStringLiteral("Распознавание"));
    tabWidget->addTab(hotkeyTab,  QStringLiteral("Управление"));
    tabWidget->addTab(outputTab,  QStringLiteral("Вывод"));

    // --- Кнопки -----------------------------------------------------------------
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveBtn   = new QPushButton(QStringLiteral("Сохранить"));
    m_cancelBtn = new QPushButton(QStringLiteral("Отмена"));
    connect(m_saveBtn,   &QPushButton::clicked, this, &SettingsDialog::saveSettings);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(tabWidget);
    mainLayout->addLayout(btnLayout);

    loadSettings();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::loadSettings()
{
    const ConfigManager cfg;

    const QString active = cfg.activeAsrProfileName();
    const int idx = m_asrProfileCombo->findText(active);
    if (idx >= 0) {
        m_asrProfileCombo->setCurrentIndex(idx);
    }

    const QString mode = cfg.hotkeyMode();
    m_pttRadio->setChecked(mode != QLatin1String("toggle"));
    m_toggleRadio->setChecked(mode == QLatin1String("toggle"));
    m_hotkeyEdit->setText(cfg.hotkeyKey());

    const QString target = cfg.notesStartTarget();
    m_outputNotesRadio->setChecked(target == QLatin1String("notes"));
    m_outputWindowRadio->setChecked(target != QLatin1String("notes"));
}

void SettingsDialog::saveSettings()
{
    // Ключи — ровно те, которые читает ConfigManager. Раньше здесь писались
    // hotkey/type («ptt») и output/target («window») — таких ключей никто не
    // читает: диалог выглядел рабочим, но ничего не менял.
    m_settings->setValue(QStringLiteral("asr/active"), m_asrProfileCombo->currentText());
    m_settings->setValue(QStringLiteral("hotkey/mode"),
                         m_pttRadio->isChecked() ? QStringLiteral("push_to_talk")
                                                 : QStringLiteral("toggle"));
    m_settings->setValue(QStringLiteral("hotkey/key"), m_hotkeyEdit->text().trimmed());
    m_settings->setValue(QStringLiteral("notes/start_target"),
                         m_outputNotesRadio->isChecked() ? QStringLiteral("notes")
                                                         : QStringLiteral("focus"));
    m_settings->sync();

    emit settingsApplied();
    accept();
}

void SettingsDialog::onKeyCapture()
{
    m_isCapturingKey = true;
    m_hotkeyBeforeCapture = m_hotkeyEdit->text();
    m_hotkeyEdit->setText(QStringLiteral("Нажмите клавишу... (Esc — отмена)"));
    m_hotkeyEdit->installEventFilter(this);
    m_hotkeyEdit->setFocus();
}

void SettingsDialog::finishKeyCapture(const QString& spec)
{
    m_isCapturingKey = false;
    m_hotkeyEdit->removeEventFilter(this);
    m_hotkeyEdit->setText(spec);
}

void SettingsDialog::cancelKeyCapture()
{
    m_isCapturingKey = false;
    m_hotkeyEdit->removeEventFilter(this);
    m_hotkeyEdit->setText(m_hotkeyBeforeCapture);
}

bool SettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (m_isCapturingKey && watched == m_hotkeyEdit
        && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        if (ke->key() == Qt::Key_Escape) {
            cancelKeyCapture();
            return true;
        }

        QString spec;
        const Qt::KeyboardModifiers mods = ke->modifiers();
        if (mods & Qt::ControlModifier) spec += QStringLiteral("ctrl+");
        if (mods & Qt::AltModifier)     spec += QStringLiteral("alt+");
        if (mods & Qt::ShiftModifier)   spec += QStringLiteral("shift+");
        if (mods & Qt::MetaModifier)    spec += QStringLiteral("super+");

        const QString keyName = qtKeyToEvdevName(ke->key());
        if (keyName.isEmpty()) {
            m_hotkeyEdit->setText(QStringLiteral("Не поддерживается — F1..F16, F20, "
                                                 "пробел, Tab, Enter, стрелки, "
                                                 "Scroll Lock, Pause (+ модификаторы). "
                                                 "Попробуйте ещё или Esc."));
            return true;   // продолжаем захват
        }
        spec += keyName;

        // Финальная проверка — тем же парсером, которым пользуется слушатель:
        // что не примет EvdevHotkeyListener, не должно попасть в конфиг.
        QString err;
        if (!EvdevHotkeyListener::parseKey(spec, nullptr, nullptr, &err)) {
            m_hotkeyEdit->setText(QStringLiteral("Не распознано: %1. Esc — отмена.").arg(err));
            return true;
        }
        finishKeyCapture(spec);
        return true;
    }
    return QDialog::eventFilter(watched, event);
}
