#include "ui/TrayIcon.h"
#include "core/ApplicationController.h"

#include <QDebug>

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QTimer>

TrayIcon::TrayIcon(ApplicationController* controller, QWidget* parent)
    : QSystemTrayIcon(parent)
    , m_controller(controller)
{
    buildMenu();
    connectSignals();
    updateIcon(m_controller ? m_controller->mode() : Mode::Off);

    // Ошибки, возникшие в конструкторе ApplicationController, сигналом мы уже
    // не получим (подписка появилась позже). Догоняем их отложенно — к этому
    // моменту значок в лотке уже создан и уведомление будет кому показать.
    if (m_controller && !m_controller->lastError().isEmpty()) {
        const QString msg = m_controller->lastError();
        QTimer::singleShot(300, this, [this, msg]() { onError(msg); });
    }
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

    // --- Проверка микрофона: запись тракта в WAV ---
    m_micCheckAction = menu->addAction(tr("Проверить микрофон (запись в WAV)"));
    m_micCheckAction->setCheckable(true);
    connect(m_micCheckAction, &QAction::triggered, this, &TrayIcon::onToggleMicCheck);

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
    connect(m_controller, &ApplicationController::errorOccurred,
            this, &TrayIcon::onError);

    // Запись микрофона завершена — сообщаем, куда лёг файл
    connect(m_controller, &ApplicationController::micCheckFinished,
            this, [this](const QString& path, double seconds) {
        if (m_micCheckAction) {
            m_micCheckAction->setChecked(false);
            m_micCheckAction->setText(tr("Проверить микрофон (запись в WAV)"));
        }
        const QString msg = tr("%1 с записано в %2\nПрогнать: ./tools/vad-asr-test %3 --config "
                               "~/.config/voice-assistant/settings.ini")
                                .arg(seconds, 0, 'f', 1).arg(path, path);
        if (QSystemTrayIcon::isSystemTrayAvailable() && supportsMessages()) {
            showMessage(tr("Запись микрофона сохранена"), msg, QSystemTrayIcon::Information, 15000);
        } else {
            qInfo().noquote() << msg;
        }
    });

    // Результаты проверки правописания показываем уведомлением: список слов
    // с вариантами исправления берём у контроллера, чтобы не тащить их сигналом.
    connect(m_controller, &ApplicationController::spellcheckFinished,
            this, [this](const QString& text, const QStringList& errors) {
        Q_UNUSED(text)
        if (errors.isEmpty()) {
            return;
        }
        QStringList lines;
        for (const QString& w : errors) {
            const QStringList sug = m_controller->suggestionsFor(w);
            lines << (sug.isEmpty() ? w
                                    : QStringLiteral("%1 → %2").arg(w, sug.join(QStringLiteral(", "))));
        }
        if (QSystemTrayIcon::isSystemTrayAvailable() && supportsMessages()) {
            showMessage(QStringLiteral("Проверка правописания: %1").arg(errors.size()),
                          lines.join(QStringLiteral("\n")),
                          QSystemTrayIcon::Information, 10000);
        } else {
            qInfo().noquote() << lines.join(QStringLiteral("; "));
        }
    });
}

void TrayIcon::onToggleMicCheck()
{
    if (!m_controller) {
        return;
    }
    if (m_controller->isMicChecking()) {
        m_controller->stopMicCheck();
        if (m_micCheckAction) {
            m_micCheckAction->setChecked(false);
            m_micCheckAction->setText(tr("Проверить микрофон (запись в WAV)"));
        }
    } else {
        QString path;
        if (m_controller->startMicCheck(&path) && m_micCheckAction) {
            m_micCheckAction->setChecked(true);
            m_micCheckAction->setText(tr("Остановить запись: %1").arg(QFileInfo(path).fileName()));
        }
    }
}

void TrayIcon::onError(const QString& message)
{
    if (message.isEmpty()) {
        return;
    }
    if (QSystemTrayIcon::isSystemTrayAvailable() && supportsMessages()) {
        showMessage(QStringLiteral("voice-assistant"), message,
                    QSystemTrayIcon::Warning, 15000);
    } else {
        // нет лотка (чистый WM, ssh) — хотя бы в лог
        qWarning().noquote() << message;
    }
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