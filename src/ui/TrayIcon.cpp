#include "ui/TrayIcon.h"
#include "core/ApplicationController.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QApplication>

TrayIcon::TrayIcon(ApplicationController* controller, QWidget* parent)
    : QSystemTrayIcon(parent)
    , m_controller(controller)
{
    buildMenu();
    connectSignals();
    updateIcon(Mode::Off);
}

void TrayIcon::buildMenu()
{
    auto* menu = new QMenu();

    // --- Пункт вкл/выкл микрофона (иконка меняется динамически) ---
    m_toggleAction = menu->addAction(
        QIcon(":/icons/mic-off.svg"),
        tr("Включить микрофон")
    );
    connect(m_toggleAction, &QAction::triggered,
            this, &TrayIcon::onToggleRecording);

    menu->addSeparator();

    // --- Подменю режимов ---
    QMenu* modeMenu = menu->addMenu(tr("Режим"));

    m_modeActions[Mode::Dictation] = modeMenu->addAction(
        QIcon(":/icons/dictation.svg"), tr("Диктовка"));
    m_modeActions[Mode::Edit] = modeMenu->addAction(
        QIcon(":/icons/edit.svg"), tr("Редактирование"));
    m_modeActions[Mode::Spellcheck] = modeMenu->addAction(
        QIcon(":/icons/spellcheck.svg"), tr("Проверка орфографии"));

    for (auto it = m_modeActions.begin(); it != m_modeActions.end(); ++it) {
        it.value()->setCheckable(true);
        Mode mode = it.key();
        connect(it.value(), &QAction::triggered, this, [this, mode]() {
            m_controller->setMode(mode);
        });
    }

    menu->addSeparator();

    // --- Настройки и редактор hotwords ---
    auto* settingsAction = menu->addAction(tr("Настройки..."));
    connect(settingsAction, &QAction::triggered,
            this, &TrayIcon::onShowSettings);

    auto* hotwordsAction = menu->addAction(tr("Редактор горячих слов..."));
    connect(hotwordsAction, &QAction::triggered,
            this, &TrayIcon::onShowHotwordsEditor);

    menu->addSeparator();

    // --- Выход ---
    auto* quitAction = menu->addAction(tr("Выход"));
    connect(quitAction, &QAction::triggered,
            qApp, &QApplication::quit);

    setContextMenu(menu);
}

void TrayIcon::connectSignals()
{
    connect(m_controller, &ApplicationController::modeChanged,
            this, &TrayIcon::onModeChanged);
}

// --- Слоты ---

void TrayIcon::onToggleRecording()
{
    if (m_controller->mode() == Mode::Off) {
        m_controller->startRecording();
    } else {
        m_controller->stopRecording();
    }
}

void TrayIcon::onShowSettings()
{
    // TODO: SettingsDialog
}

void TrayIcon::onShowHotwordsEditor()
{
    // TODO: HotwordsEditor
}

void TrayIcon::onModeChanged(Mode mode)
{
    updateIcon(mode);
    updateMenu(mode);
}

// --- Приватные методы обновления ---

void TrayIcon::updateIcon(Mode mode)
{
    QString iconPath;
    switch (mode) {
        case Mode::Off:         iconPath = ":/icons/mic-off.svg";      break;
        case Mode::Dictation:   iconPath = ":/icons/dictation.svg";    break;
        case Mode::Edit:        iconPath = ":/icons/edit.svg";         break;
        case Mode::Spellcheck:  iconPath = ":/icons/spellcheck.svg";   break;
        case Mode::Error:       iconPath = ":/icons/error.svg";        break;
    }
    setIcon(QIcon(iconPath));
}

void TrayIcon::updateMenu(Mode mode)
{
    // Обновляем пункт "Вкл/Выкл микрофон"
    const bool isRecording = (mode != Mode::Off);
    if (isRecording) {
        m_toggleAction->setIcon(QIcon(":/icons/mic-on.svg"));
        m_toggleAction->setText(tr("Выключить микрофон"));
    } else {
        m_toggleAction->setIcon(QIcon(":/icons/mic-off.svg"));
        m_toggleAction->setText(tr("Включить микрофон"));
    }

    // Галки на пунктах режимов
    for (auto it = m_modeActions.begin(); it != m_modeActions.end(); ++it) {
        it.value()->setChecked(it.key() == mode);
    }
}