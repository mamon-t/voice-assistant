#pragma once

#include <QSystemTrayIcon>
#include <QMap>
#include "core/Mode.h"
#include "core/OutputTarget.h"
#include "ui/SettingsDialog.h"

class QAction;
class ApplicationController;

class TrayIcon : public QSystemTrayIcon
{
    Q_OBJECT

public:
    explicit TrayIcon(ApplicationController* controller, QWidget* parent = nullptr);

private slots:
    void onToggleRecording();
    void onShowSettings();
    void onShowHotwordsEditor();
    void onShowCommandsEditor();
    void onModeChanged(Mode mode);
    void onError(const QString& message);
    void onToggleMicCheck();
    void onToggleNotesTarget();
    void onOpenNotesFile();
    void onOutputTargetChanged(OutputTarget target);

private:
    void buildMenu();
    void connectSignals();
    void updateIcon(Mode mode);
    void updateMenu(Mode mode);
    // Тултип лотка — единственный постоянно видимый индикатор состояния:
    // режим (ожидание / ● ЗАПИСЬ / ошибка), цель вывода и активная модель.
    void updateTooltip();
    // Синхронизирует пункты заметок с состоянием контроллера БЕЗ уведомления:
    // вызывается при старте и из updateMenu(), чтобы не показывать всплывающее
    // окно каждый раз, когда меню перерисовывается.
    void syncNotesAction();
    // Один диалог настроек на все случаи: пункты меню лотка открывают его
    // на нужной вкладке, а применение настроек (settingsApplied) описано
    // в одном месте.
    void execSettingsDialog(SettingsDialog::Tab tab);

    ApplicationController* m_controller;
    QAction* m_toggleAction = nullptr;
    QAction* m_micCheckAction = nullptr;
    QAction* m_notesAction = nullptr;       // куда писать: окно или файл заметок
    QAction* m_openNotesAction = nullptr;
    QMap<Mode, QAction*> m_modeActions;
};