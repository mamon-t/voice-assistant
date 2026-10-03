#pragma once

#include <QSystemTrayIcon>
#include <QMap>
#include "core/Mode.h"

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
    void onModeChanged(Mode mode);
    void onError(const QString& message);

private:
    void buildMenu();
    void connectSignals();
    void updateIcon(Mode mode);
    void updateMenu(Mode mode);

    ApplicationController* m_controller;
    QAction* m_toggleAction = nullptr;
    QMap<Mode, QAction*> m_modeActions;
};