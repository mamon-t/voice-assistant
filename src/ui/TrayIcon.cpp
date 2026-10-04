#include "ui/TrayIcon.h"
#include "core/ApplicationController.h"
#include "config/ConfigManager.h"
#include "ui/SettingsDialog.h"

#include <QDebug>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>
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
    updateTooltip();
    syncNotesAction();

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

    // --- Цель вывода: активное окно или файл заметок ---
    // Заметки нужны, чтобы диктовать по ходу работы с файлом: результат уходит
    // в файл, а не в окно, поэтому клавиатура, фокус и буфер обмена свободны —
    // можно продолжать печатать руками.
    m_notesAction = menu->addAction(tr("Писать в файл заметок"));
    m_notesAction->setCheckable(true);
    m_notesAction->setToolTip(tr("Распознанный текст дописывается в файл заметок, "
                                 "а не вставляется в активное окно"));
    connect(m_notesAction, &QAction::triggered, this, &TrayIcon::onToggleNotesTarget);

    m_openNotesAction = menu->addAction(tr("Открыть файл заметок"));
    connect(m_openNotesAction, &QAction::triggered, this, &TrayIcon::onOpenNotesFile);

    menu->addSeparator();

    // --- Настройки: один диалог, пункты меню открывают нужную вкладку ---
    auto* settingsAction = menu->addAction(tr("Настройки..."));
    connect(settingsAction, &QAction::triggered,
            this, &TrayIcon::onShowSettings);

    auto* hotwordsAction = menu->addAction(tr("Голосовые подсказки..."));
    hotwordsAction->setToolTip(tr("Слова и фразы, которые модель должна узнавать лучше"));
    connect(hotwordsAction, &QAction::triggered,
            this, &TrayIcon::onShowHotwordsEditor);

    auto* commandsAction = menu->addAction(tr("Голосовые команды..."));
    commandsAction->setToolTip(tr("Что понимает помощник: встроенные команды и свои фразы"));
    connect(commandsAction, &QAction::triggered,
            this, &TrayIcon::onShowCommandsEditor);

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
    connect(m_controller, &ApplicationController::outputTargetChanged,
            this, &TrayIcon::onOutputTargetChanged);

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
    execSettingsDialog(SettingsDialog::TabAsr);
}

void TrayIcon::onShowHotwordsEditor()
{
    execSettingsDialog(SettingsDialog::TabHotwords);
}

void TrayIcon::onShowCommandsEditor()
{
    execSettingsDialog(SettingsDialog::TabCommands);
}

void TrayIcon::execSettingsDialog(SettingsDialog::Tab tab)
{
    if (!m_controller) {
        return;
    }
    SettingsDialog dialog(nullptr, tab);
    // settingsApplied испускается после записи ini/файлов — применяем на лету:
    // модель, цель вывода, настройки инжектора, хоткей, подсказки и команды
    // перечитываются без перезапуска. И ОБЯЗАТЕЛЬНО говорим пользователю, что
    // применилось: молчаливое применение настроек — то самое «выглядит рабочим,
    // но не работает», с которым проект борется с итерации 10.
    connect(&dialog, &SettingsDialog::settingsApplied, this, [this]() {
        const ConfigManager cfg;   // свежий экземпляр: читает уже сохранённый ini

        QString modelReport;
        bool modelFailed = false;
        const QString profile = cfg.activeAsrProfileName();
        if (profile != m_controller->activeAsrProfile()) {
            // Пересоздание пайплайна занимает 1–2 с: показываем курсор
            // ожидания, иначе UI выглядит подвисшим.
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool ok = m_controller->switchAsrProfile(profile);
            QApplication::restoreOverrideCursor();
            modelFailed = !ok;
            modelReport = ok
                ? tr("Модель ASR «%1» перезагружена.").arg(profile)
                : tr("Модель ASR «%1» НЕ переключена: %2")
                      .arg(profile, m_controller->lastError());
        }

        // Состояние применяем синхронно: к моменту возврата из диалога всё
        // уже действует.
        m_controller->setOutputTarget(
            cfg.notesStartTarget() == QLatin1String("notes") ? OutputTarget::Notes
                                                             : OutputTarget::Focus);
        m_controller->reloadOutputSettings();
        m_controller->reloadHotkey();
        m_controller->reloadHotwords();
        m_controller->reloadCommands();
        syncNotesAction();

        // Тултип и уведомление — ОТЛОЖЕННО, на следующем витке event loop:
        // здесь мы всё ещё внутри saveSettings() модального диалога (exec()
        // не вернулся). Обращения к лотковому мосту (setToolTip/showMessage
        // у QSystemTrayIcon на StatusNotifier) из стека сигнала сохранения,
        // пока модальное окно живо и вот-вот начнёт разрушаться, — плохая
        // примета; в отложенном виде они выполняются уже после уничтожения
        // диалога.
        const QString body = modelReport.isEmpty()
            ? tr("Изменения вступили в силу без перезапуска.")
            : modelReport;
        QTimer::singleShot(0, this, [this, body, modelFailed]() {
            updateTooltip();
            if (QSystemTrayIcon::isSystemTrayAvailable() && supportsMessages()) {
                showMessage(tr("Настройки применены"), body,
                            modelFailed ? QSystemTrayIcon::Warning
                                        : QSystemTrayIcon::Information,
                            modelFailed ? 15000 : 4000);
            } else {
                qInfo().noquote() << QStringLiteral("Настройки применены: %1").arg(body);
            }
        });
    });
    dialog.exec();
}

void TrayIcon::onModeChanged(Mode mode)
{
    updateIcon(mode);
    updateMenu(mode);
    updateTooltip();
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

    syncNotesAction();
}

void TrayIcon::updateTooltip()
{
    if (!m_controller) {
        setToolTip(tr("Голосовой помощник"));
        return;
    }
    QString state;
    switch (m_controller->mode()) {
    case Mode::Off:        state = tr("ожидание (микрофон выключен)"); break;
    case Mode::Dictation:  state = tr("● ЗАПИСЬ — диктовка");          break;
    case Mode::Edit:       state = tr("● ЗАПИСЬ — редактирование");    break;
    case Mode::Spellcheck: state = tr("● ЗАПИСЬ — проверка правописания"); break;
    case Mode::Error:      state = tr("ОШИБКА — см. последнее уведомление"); break;
    }
    const bool notes = (m_controller->outputTarget() == OutputTarget::Notes);
    const QString target = notes
        ? tr("файл заметок %1").arg(m_controller->notesFilePath())
        : tr("активное окно (привязывается в начале записи)");
    setToolTip(tr("Голосовой помощник\n%1\nКуда писать: %2\nМодель: %3")
                   .arg(state, target, m_controller->activeAsrProfile()));
}

void TrayIcon::syncNotesAction()
{
    if (!m_controller) {
        return;
    }
    const bool notes = (m_controller->outputTarget() == OutputTarget::Notes);
    const QString path = m_controller->notesFilePath();

    if (m_notesAction) {
        m_notesAction->setChecked(notes);
        m_notesAction->setText(notes ? tr("Писать в активное окно")
                                     : tr("Писать в файл заметок"));
        m_notesAction->setEnabled(notes || !path.isEmpty());
    }
    if (m_openNotesAction) {
        m_openNotesAction->setEnabled(!path.isEmpty());
        m_openNotesAction->setText(path.isEmpty()
            ? tr("Открыть файл заметок")
            : tr("Открыть заметки: %1").arg(QFileInfo(path).fileName()));
    }
}

void TrayIcon::onToggleNotesTarget()
{
    if (!m_controller) {
        return;
    }
    // -> outputTargetChanged -> onOutputTargetChanged (галка и уведомление)
    m_controller->toggleOutputTarget();
}

void TrayIcon::onOutputTargetChanged(OutputTarget target)
{
    syncNotesAction();
    updateTooltip();

    // Уведомление обязательно: другого индикатора цели вывода нет, а ошибка
    // «диктовал в редактор, а текст ушёл в файл» стоит дорого.
    const bool notes = (target == OutputTarget::Notes);
    const QString title = notes ? tr("Заметки") : tr("Диктовка");
    const QString msg = notes
        ? tr("Текст дописывается в %1\nКлавиатура, фокус и буфер обмена не трогаются.")
              .arg(m_controller ? m_controller->notesFilePath() : QString())
        : tr("Текст вставляется в активное окно.");

    if (QSystemTrayIcon::isSystemTrayAvailable() && supportsMessages()) {
        showMessage(title, msg, QSystemTrayIcon::Information, 6000);
    } else {
        qInfo().noquote() << title << QStringLiteral(":") << msg;
    }
}

void TrayIcon::onOpenNotesFile()
{
    const QString path = m_controller ? m_controller->notesFilePath() : QString();
    if (path.isEmpty()) {
        onError(tr("Файл заметок не задан (проверьте [notes] enabled)"));
        return;
    }
    if (!QFileInfo::exists(path)) {
        onError(tr("Файла заметок ещё нет: %1\nОн появится после первой записи.").arg(path));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        onError(tr("Не удалось открыть %1").arg(path));
    }
}