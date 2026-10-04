#include "SettingsDialog.h"

#include "commands/CommandDictionary.h"
#include "commands/CommandParser.h"
#include "config/AsrProfile.h"
#include "config/ConfigManager.h"
#include "core/Command.h"
#include "input/EvdevHotkeyListener.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
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

// Суммарный размер файлов модели профиля (чего нет на диске — не считаем).
qint64 profileModelSize(const AsrProfile& p)
{
    qint64 total = 0;
    const auto add = [&total](const QString& path) {
        if (path.isEmpty()) {
            return;
        }
        const QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            total += fi.size();
        }
    };
    switch (p.engine) {
    case AsrProfile::Engine::Transducer:
        add(p.encoderPath); add(p.decoderPath); add(p.joinerPath);
        break;
    case AsrProfile::Engine::NemoCtc:
        add(p.ctcModelPath);
        break;
    case AsrProfile::Engine::Whisper:
        add(p.encoderPath); add(p.decoderPath);
        break;
    case AsrProfile::Engine::Unknown:
        break;
    }
    add(p.tokensPath);
    return total;
}

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("0 МБ");
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 КБ").arg(bytes / 1024.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 МБ").arg(bytes / (1024.0 * 1024.0), 0, 'f', 0);
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent, Tab initial)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Настройки голосового помощника"));
    resize(600, 520);

    // Тот же файл, что читает ConfigManager. Запись — через QSettings,
    // чтение — через ConfigManager (он знает про грабли разбора ini).
    const ConfigManager cfg;
    m_settings = new QSettings(cfg.settingsPath(), QSettings::IniFormat, this);
    m_hotwordsPath = cfg.userHotwordsPath();
    m_commandsPath = cfg.customCommandsPath();

    auto* tabWidget = new QTabWidget(this);
    auto* style = this->style();

    tabWidget->addTab(buildAsrTab(),
                      style->standardIcon(QStyle::SP_FileDialogContentsView),
                      QStringLiteral("Распознавание"));
    tabWidget->addTab(buildControlTab(),
                      style->standardIcon(QStyle::SP_FileDialogDetailedView),
                      QStringLiteral("Управление"));
    tabWidget->addTab(buildOutputTab(),
                      style->standardIcon(QStyle::SP_FileDialogListView),
                      QStringLiteral("Вывод"));
    tabWidget->addTab(buildHotwordsTab(),
                      style->standardIcon(QStyle::SP_FileDialogInfoView),
                      QStringLiteral("Подсказки"));
    tabWidget->addTab(buildCommandsTab(),
                      style->standardIcon(QStyle::SP_ComputerIcon),
                      QStringLiteral("Команды"));
    tabWidget->setCurrentIndex(static_cast<int>(initial));

    // --- Кнопки -----------------------------------------------------------------
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveBtn   = new QPushButton(style->standardIcon(QStyle::SP_DialogApplyButton),
                                  QStringLiteral("Сохранить"));
    m_cancelBtn = new QPushButton(style->standardIcon(QStyle::SP_DialogCancelButton),
                                  QStringLiteral("Отмена"));
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

// ---------------------------------------------------------------------------
// Вкладки
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::buildAsrTab()
{
    const ConfigManager cfg;
    auto* tab = new QWidget();
    auto* lay = new QVBoxLayout(tab);
    auto* form = new QFormLayout();

    m_asrProfileCombo = new QComboBox();
    // Профили — из [asr] profiles, без хардкода: список должен совпадать
    // с тем, что реально настроено в ini (и с --check).
    m_asrProfileCombo->addItems(cfg.asrProfileNames());
    form->addRow(QStringLiteral("Модель ASR:"), m_asrProfileCombo);
    lay->addLayout(form);

    // Статус выбранного профиля: движок, размер, готовность. Ошибку модели
    // видно ДО применения, а не по падению диктовки.
    m_asrStatusLabel = new QLabel();
    m_asrStatusLabel->setWordWrap(true);
    m_asrStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_asrStatusLabel);

    connect(m_asrProfileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onAsrProfileChanged);

    lay->addWidget(new QLabel(
        QStringLiteral("Переключение применяется сразу: модель перезагружается "
                       "за 1–2 с (в это время диктовка недоступна).")));
    lay->addStretch();
    return tab;
}

QWidget* SettingsDialog::buildControlTab()
{
    auto* tab = new QWidget();
    auto* lay = new QVBoxLayout(tab);

    auto* modeGroup = new QGroupBox(QStringLiteral("Режим активации"));
    auto* modeLayout = new QVBoxLayout(modeGroup);
    m_pttRadio    = new QRadioButton(QStringLiteral("Push-to-talk (держать клавишу, пока говорите)"));
    m_toggleRadio = new QRadioButton(QStringLiteral("Toggle (нажал — запись, отжал — остановка)"));
    modeLayout->addWidget(m_pttRadio);
    modeLayout->addWidget(m_toggleRadio);
    lay->addWidget(modeGroup);

    auto* keyLayout = new QHBoxLayout();
    m_hotkeyEdit = new QLineEdit();
    m_hotkeyEdit->setReadOnly(true);
    m_hotkeyEdit->setPlaceholderText(QStringLiteral("Нажмите «Захватить», затем клавишу..."));
    m_captureBtn = new QPushButton(QStringLiteral("Захватить"));
    connect(m_captureBtn, &QPushButton::clicked, this, &SettingsDialog::onKeyCapture);
    keyLayout->addWidget(new QLabel(QStringLiteral("Глобальная клавиша:")));
    keyLayout->addWidget(m_hotkeyEdit);
    keyLayout->addWidget(m_captureBtn);
    lay->addLayout(keyLayout);

    lay->addWidget(new QLabel(
        QStringLiteral("Поддерживаются: F1–F16, F20, пробел, Tab, Enter, стрелки,\n"
                       "Scroll Lock, Pause и комбинации с ctrl/alt/shift/super.\n"
                       "Буквы и цифры не годятся: хоткей не должен мешать печатать.\n"
                       "Совет: клавиша доходит и до активного приложения — в терминале\n"
                       "F8 напечатает «^[[19~». Берите то, что ничего не печатает:\n"
                       "Scroll Lock, Pause, правый Ctrl, F13+ или комбинацию.\n"
                       "Нужны права на /dev/input: sudo usermod -aG input $USER.")));
    lay->addStretch();
    return tab;
}

QWidget* SettingsDialog::buildOutputTab()
{
    auto* tab = new QWidget();
    auto* lay = new QVBoxLayout(tab);

    auto* targetGroup = new QGroupBox(QStringLiteral("Цель вывода при старте"));
    auto* targetLayout = new QVBoxLayout(targetGroup);
    m_outputWindowRadio = new QRadioButton(
        QStringLiteral("Активное окно (буфер обмена + Ctrl+V или xdotool)"));
    m_outputNotesRadio = new QRadioButton(
        QStringLiteral("Файл заметок (~/.local/share/voice-assistant/notes/)"));
    targetLayout->addWidget(m_outputWindowRadio);
    targetLayout->addWidget(m_outputNotesRadio);
    lay->addWidget(targetGroup);
    lay->addWidget(new QLabel(
        QStringLiteral("Переключать цель на лету можно голосом («заметка» / «в редактор»)\n"
                       "или из меню лотка; здесь задаётся только стартовое значение.")));

    auto* methodGroup = new QGroupBox(QStringLiteral("Вставка в окно"));
    auto* methodForm = new QFormLayout(methodGroup);

    m_methodCombo = new QComboBox();
    m_methodCombo->addItem(QStringLiteral("auto — буфер обмена + Ctrl+V, иначе xdotool type"),
                           QStringLiteral("auto"));
    m_methodCombo->addItem(QStringLiteral("clipboard — всегда буфер обмена + Ctrl+V"),
                           QStringLiteral("clipboard"));
    m_methodCombo->addItem(QStringLiteral("xdotool — всегда посимвольная печать (медленно)"),
                           QStringLiteral("xdotool"));
    methodForm->addRow(QStringLiteral("Способ:"), m_methodCombo);

    m_pinWindowCheck = new QCheckBox(
        QStringLiteral("Привязывать вставку к окну, активному в начале записи"));
    connect(m_pinWindowCheck, &QCheckBox::toggled, this, &SettingsDialog::onPinWindowToggled);
    methodForm->addRow(QString(), m_pinWindowCheck);

    m_pinGroup = new QGroupBox();
    m_pinGroup->setFlat(true);
    auto* pinForm = new QFormLayout(m_pinGroup);
    pinForm->setContentsMargins(24, 0, 0, 0);

    m_pinModeCombo = new QComboBox();
    m_pinModeCombo->addItem(
        QStringLiteral("activate — активировать окно, печать настоящими событиями (работает везде)"),
        QStringLiteral("activate"));
    m_pinModeCombo->addItem(
        QStringLiteral("sendevent — не трогать фокус (приложение должно принимать синтетику)"),
        QStringLiteral("sendevent"));
    pinForm->addRow(QStringLiteral("Режим:"), m_pinModeCombo);

    m_pinActivateSpin = new QSpinBox();
    m_pinActivateSpin->setRange(0, 2000);
    m_pinActivateSpin->setSingleStep(10);
    m_pinActivateSpin->setSuffix(QStringLiteral(" мс"));
    pinForm->addRow(QStringLiteral("Пауза после активации:"), m_pinActivateSpin);

    m_pinRestoreCheck = new QCheckBox(QStringLiteral("Возвращать фокус прежнему окну после вставки"));
    pinForm->addRow(QString(), m_pinRestoreCheck);
    methodForm->addRow(m_pinGroup);

    lay->addWidget(methodGroup);
    lay->addStretch();
    return tab;
}

QWidget* SettingsDialog::buildHotwordsTab()
{
    auto* tab = new QWidget();
    auto* lay = new QVBoxLayout(tab);

    lay->addWidget(new QLabel(
        QStringLiteral("Файл: %1\n"
                       "Редкие слова, имена и термины: модель начинает узнавать их лучше\n"
                       "(для transducer-профилей — настоящий контекстный бустинг).\n"
                       "Слишком длинный список размывает внимание: держите самое нужное.")
            .arg(m_hotwordsPath)));

    m_hotwordsList = new QListWidget();
    // Двойной клик — правка на месте: опечатка в подсказке стоит точности
    // распознавания, а пересоздавать слово ради одной буквы утомительно.
    m_hotwordsList->setEditTriggers(QAbstractItemView::DoubleClicked
                                    | QAbstractItemView::EditKeyPressed);
    lay->addWidget(m_hotwordsList);

    m_hotwordsCountLabel = new QLabel();
    lay->addWidget(m_hotwordsCountLabel);
    connect(m_hotwordsList, &QListWidget::itemChanged,
            this, [this]() { updateHotwordCount(); });
    connect(m_hotwordsList->model(), &QAbstractItemModel::rowsInserted,
            this, [this]() { updateHotwordCount(); });
    connect(m_hotwordsList->model(), &QAbstractItemModel::rowsRemoved,
            this, [this]() { updateHotwordCount(); });

    auto* inputLayout = new QHBoxLayout();
    m_hotwordInput = new QLineEdit();
    m_hotwordInput->setPlaceholderText(QStringLiteral("Новое слово или фраза..."));
    auto* addBtn = new QPushButton(QStringLiteral("Добавить"));
    m_hotwordRemoveBtn = new QPushButton(QStringLiteral("Удалить"));
    connect(addBtn,   &QPushButton::clicked,     this, &SettingsDialog::addHotword);
    connect(m_hotwordRemoveBtn, &QPushButton::clicked, this, &SettingsDialog::removeHotword);
    connect(m_hotwordInput, &QLineEdit::returnPressed, this, &SettingsDialog::addHotword);
    connect(m_hotwordsList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_hotwordRemoveBtn->setEnabled(!m_hotwordsList->selectedItems().isEmpty());
    });
    m_hotwordRemoveBtn->setEnabled(false);

    inputLayout->addWidget(m_hotwordInput);
    inputLayout->addWidget(addBtn);
    inputLayout->addWidget(m_hotwordRemoveBtn);
    lay->addLayout(inputLayout);
    return tab;
}

QWidget* SettingsDialog::buildCommandsTab()
{
    auto* tab = new QWidget();
    auto* lay = new QVBoxLayout(tab);

    lay->addWidget(new QLabel(QStringLiteral(
        "Командой считается фраза, совпавшая со словарём ЦЕЛИКОМ.\n"
        "Встроенные команды заданы в коде — они только для чтения; чтобы\n"
        "переопределить фразу, скопируйте её в «свои» и измените левую часть.")));

    // --- встроенные (read-only) ---
    auto* builtinGroup = new QGroupBox(QStringLiteral("Встроенные команды"));
    auto* builtinLay = new QVBoxLayout(builtinGroup);

    m_builtinFilter = new QLineEdit();
    m_builtinFilter->setPlaceholderText(QStringLiteral("Фильтр: «удали», «режим», «точка»..."));
    connect(m_builtinFilter, &QLineEdit::textChanged,
            this, &SettingsDialog::filterBuiltinCommands);
    builtinLay->addWidget(m_builtinFilter);

    m_builtinTable = new QTableWidget(0, 2);
    m_builtinTable->setHorizontalHeaderLabels({QStringLiteral("Фраза"),
                                               QStringLiteral("Действие")});
    m_builtinTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_builtinTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_builtinTable->verticalHeader()->setVisible(false);
    m_builtinTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_builtinTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_builtinTable->setSelectionMode(QAbstractItemView::SingleSelection);

    // Источник истины — тот же словарь, которым пользуется приложение:
    // свежий CommandDictionary + loadDefaults(), без своего файла.
    {
        CommandDictionary dict;
        dict.loadDefaults();
        const QStringList phrases = dict.phrases();
        m_builtinTable->setRowCount(phrases.size());
        int row = 0;
        for (const QString& phrase : phrases) {
            const auto cmd = dict.find(phrase);   // phrases() уже нормализованы
            auto* phraseItem = new QTableWidgetItem(phrase);
            phraseItem->setData(Qt::UserRole, cmd ? commandToSpec(*cmd) : QString());
            m_builtinTable->setItem(row, 0, phraseItem);
            m_builtinTable->setItem(row, 1, new QTableWidgetItem(
                cmd ? commandDescription(*cmd) : QStringLiteral("?")));
            ++row;
        }
    }
    builtinLay->addWidget(m_builtinTable);

    m_copyBuiltinBtn = new QPushButton(
        QStringLiteral("Скопировать выбранную в «свои» (чтобы изменить фразу)"));
    connect(m_copyBuiltinBtn, &QPushButton::clicked,
            this, &SettingsDialog::copyBuiltinToCustom);
    builtinLay->addWidget(m_copyBuiltinBtn);
    lay->addWidget(builtinGroup);

    // --- свои (файл [commands] file) ---
    auto* customGroup = new QGroupBox(
        QStringLiteral("Свои команды — файл %1").arg(m_commandsPath));
    auto* customLay = new QVBoxLayout(customGroup);

    m_customList = new QListWidget();
    m_customList->setEditTriggers(QAbstractItemView::DoubleClicked
                                  | QAbstractItemView::EditKeyPressed);
    customLay->addWidget(m_customList);
    customLay->addWidget(new QLabel(
        QStringLiteral("Формат: «фраза = тип[:аргумент]», типы: set-mode:{dictation|edit|"
                       "spellcheck|off}, set-target:{focus|notes}, delete-word, delete-line, "
                       "new-line, space, punctuation:<символ>.\n"
                       "Своя фраза перекрывает встроенную с тем же действием; при сохранении "
                       "файл перезаписывается (комментарии не сохраняются), битые строки "
                       "сохранить не дадим.")));

    auto* customBtns = new QHBoxLayout();
    auto* addBtn = new QPushButton(QStringLiteral("Добавить"));
    m_customRemoveBtn = new QPushButton(QStringLiteral("Удалить"));
    connect(addBtn, &QPushButton::clicked, this, &SettingsDialog::addCustomCommand);
    connect(m_customRemoveBtn, &QPushButton::clicked, this, &SettingsDialog::removeCustomCommand);
    connect(m_customList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_customRemoveBtn->setEnabled(!m_customList->selectedItems().isEmpty());
    });
    m_customRemoveBtn->setEnabled(false);
    customBtns->addStretch();
    customBtns->addWidget(addBtn);
    customBtns->addWidget(m_customRemoveBtn);
    customLay->addLayout(customBtns);
    lay->addWidget(customGroup);

    // --- поведение команд ---
    m_editInDictationCheck = new QCheckBox(
        QStringLiteral("Выполнять команды правки в режиме диктовки ([commands] editing_in_dictation)"));
    m_editInDictationCheck->setToolTip(
        QStringLiteral("Выключено: в диктовке работают только команды режима и цели,\n"
                       "а сказанное «удали слово» попадает в текст как есть."));
    m_notesVoiceCheck = new QCheckBox(
        QStringLiteral("Голосовое переключение цели: «заметка» / «в редактор» ([notes] voice_commands)"));
    m_notesVoiceCheck->setToolTip(
        QStringLiteral("Слово «заметка» встречается в обычной речи — если мешает,\n"
                       "выключите: цель останется доступной из лотка и по D-Bus."));
    lay->addWidget(m_editInDictationCheck);
    lay->addWidget(m_notesVoiceCheck);
    return tab;
}

// ---------------------------------------------------------------------------
// Загрузка / сохранение
// ---------------------------------------------------------------------------

void SettingsDialog::loadSettings()
{
    const ConfigManager cfg;

    // --- ASR ---
    const QString active = cfg.activeAsrProfileName();
    const int idx = m_asrProfileCombo->findText(active);
    if (idx >= 0) {
        m_asrProfileCombo->setCurrentIndex(idx);
    }
    onAsrProfileChanged(m_asrProfileCombo->currentIndex());

    // --- Хоткей ---
    const QString mode = cfg.hotkeyMode();
    m_pttRadio->setChecked(mode != QLatin1String("toggle"));
    m_toggleRadio->setChecked(mode == QLatin1String("toggle"));
    m_hotkeyEdit->setText(cfg.hotkeyKey());

    // --- Вывод ---
    const QString target = cfg.notesStartTarget();
    m_outputNotesRadio->setChecked(target == QLatin1String("notes"));
    m_outputWindowRadio->setChecked(target != QLatin1String("notes"));

    const int mi = m_methodCombo->findData(cfg.injectorMethod());
    m_methodCombo->setCurrentIndex(mi >= 0 ? mi : 0);

    m_pinWindowCheck->setChecked(cfg.pinWindow());
    const int pi = m_pinModeCombo->findData(cfg.pinMode());
    m_pinModeCombo->setCurrentIndex(pi >= 0 ? pi : 0);
    m_pinActivateSpin->setValue(cfg.pinActivateMs());
    m_pinRestoreCheck->setChecked(cfg.pinRestoreFocus());
    onPinWindowToggled(m_pinWindowCheck->isChecked());

    // --- Подсказки ---
    m_hotwordsList->clear();
    m_hotwordsList->addItems(cfg.loadHotwords(m_hotwordsPath));
    updateHotwordCount();

    // --- Команды: свой файл и поведение ---
    m_customList->clear();
    QFile f(m_commandsPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        in.setCodec("UTF-8");   // грабля №6: без setCodec кириллица едет в Latin-1
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
                m_customList->addItem(line);
            }
        }
    }
    m_editInDictationCheck->setChecked(cfg.editingCommandsInDictation());
    m_notesVoiceCheck->setChecked(cfg.notesVoiceCommands());
}

void SettingsDialog::saveSettings()
{
    // Сначала валидация команд: если строки битые, НЕ сохраняем ничего —
    // иначе получилось бы «половина настроек применилась, половина нет».
    if (!saveCustomCommands()) {
        return;
    }
    if (!saveHotwords()) {
        QMessageBox::warning(this, QStringLiteral("Подсказки"),
                             QStringLiteral("Не удалось записать файл %1 — проверьте права.")
                                 .arg(m_hotwordsPath));
        return;
    }

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
    m_settings->setValue(QStringLiteral("output/method"),
                         m_methodCombo->currentData().toString());
    m_settings->setValue(QStringLiteral("output/pin_window"), m_pinWindowCheck->isChecked());
    m_settings->setValue(QStringLiteral("output/pin_mode"),
                         m_pinModeCombo->currentData().toString());
    m_settings->setValue(QStringLiteral("output/pin_activate_ms"), m_pinActivateSpin->value());
    m_settings->setValue(QStringLiteral("output/pin_restore_focus"),
                         m_pinRestoreCheck->isChecked());
    m_settings->setValue(QStringLiteral("commands/editing_in_dictation"),
                         m_editInDictationCheck->isChecked());
    m_settings->setValue(QStringLiteral("notes/voice_commands"),
                         m_notesVoiceCheck->isChecked());
    m_settings->sync();

    emit settingsApplied();
    accept();
}

bool SettingsDialog::saveHotwords()
{
    QStringList words;
    words.reserve(m_hotwordsList->count());
    for (int i = 0; i < m_hotwordsList->count(); ++i) {
        const QString t = m_hotwordsList->item(i)->text().trimmed();
        if (!t.isEmpty()) {
            words << t;
        }
    }
    const ConfigManager cfg;
    cfg.saveHotwords(m_hotwordsPath, words);   // UTF-8, по слову в строке
    return QFile::exists(m_hotwordsPath);
}

bool SettingsDialog::saveCustomCommands()
{
    // Валидация — той же CommandDictionary::parseLine, которой файл будет
    // загружаться: что примет диалог, то гарантированно загрузится. Заодно
    // строки нормализуются через commandToSpec (round-trip через парсер).
    QStringList lines;
    QStringList bad;
    for (int i = 0; i < m_customList->count(); ++i) {
        const QString raw = m_customList->item(i)->text();
        QString phrase;
        Command cmd;
        QString err;
        if (CommandDictionary::parseLine(raw, &phrase, &cmd, &err)) {
            lines << QStringLiteral("%1 = %2").arg(phrase, commandToSpec(cmd));
        } else if (!err.isEmpty()) {
            bad << QStringLiteral("«%1» — %2").arg(raw.trimmed(), err);
        }
    }
    if (!bad.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Битые строки в командах"),
                             QStringLiteral("Исправьте или удалите строки — ничего не сохранено:\n\n%1")
                                 .arg(bad.join(QLatin1Char('\n'))));
        return false;
    }

    QDir().mkpath(QFileInfo(m_commandsPath).absolutePath());
    QFile f(m_commandsPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Команды"),
                             QStringLiteral("Не удалось записать файл %1 — проверьте права.")
                                 .arg(m_commandsPath));
        return false;
    }
    QTextStream out(&f);
    out.setCodec("UTF-8");   // грабля №6
    out << QStringLiteral("# Свои голосовые команды: «фраза = тип[:аргумент]».\n"
                          "# Файл ведётся из диалога настроек, вкладка «Команды».\n");
    for (const QString& l : std::as_const(lines)) {
        out << l << '\n';
    }
    return true;
}

// ---------------------------------------------------------------------------
// Слоты вкладок
// ---------------------------------------------------------------------------

void SettingsDialog::onAsrProfileChanged(int)
{
    const QString name = m_asrProfileCombo->currentText();
    if (name.isEmpty()) {
        m_asrStatusLabel->setText(QStringLiteral("Профили не найдены — проверьте "
                                                  "[asr] profiles в settings.ini"));
        return;
    }
    const ConfigManager cfg;
    const AsrProfile p = cfg.asrProfile(name);

    QString err;
    const bool valid = p.isValid(&err);
    const QString size = humanSize(profileModelSize(p));
    if (valid) {
        m_asrStatusLabel->setText(QStringLiteral("✔ %1 · %2 · %3 · готов к работе")
                                      .arg(name, AsrProfile::engineToString(p.engine), size));
    } else {
        m_asrStatusLabel->setText(QStringLiteral("✖ %1 · %2 · НЕ готов: %3")
                                      .arg(name, AsrProfile::engineToString(p.engine),
                                           err.split(QLatin1Char('\n')).first()));
    }
}

void SettingsDialog::onPinWindowToggled(bool on)
{
    m_pinGroup->setEnabled(on);
}

void SettingsDialog::updateHotwordCount()
{
    m_hotwordsCountLabel->setText(
        QStringLiteral("Слов: %1. Правка — двойным кликом; в силу вступает после «Сохранить».")
            .arg(m_hotwordsList->count()));
}

void SettingsDialog::addHotword()
{
    const QString word = m_hotwordInput->text().trimmed();
    if (!word.isEmpty()) {
        if (m_hotwordsList->findItems(word, Qt::MatchExactly).isEmpty()) {
            m_hotwordsList->addItem(word);
        }
        m_hotwordInput->clear();
    }
}

void SettingsDialog::removeHotword()
{
    qDeleteAll(m_hotwordsList->selectedItems());
}

void SettingsDialog::filterBuiltinCommands(const QString& text)
{
    const QString needle = text.trimmed().toLower();
    for (int row = 0; row < m_builtinTable->rowCount(); ++row) {
        const QString phrase = m_builtinTable->item(row, 0)->text().toLower();
        const QString action = m_builtinTable->item(row, 1)->text().toLower();
        m_builtinTable->setRowHidden(row, !needle.isEmpty()
                                         && !phrase.contains(needle)
                                         && !action.contains(needle));
    }
}

void SettingsDialog::copyBuiltinToCustom()
{
    const int row = m_builtinTable->currentRow();
    if (row < 0) {
        return;
    }
    const QString phrase = m_builtinTable->item(row, 0)->text();
    const QString spec   = m_builtinTable->item(row, 0)->data(Qt::UserRole).toString();
    const QString line   = QStringLiteral("%1 = %2").arg(phrase, spec);
    if (!m_customList->findItems(line, Qt::MatchExactly).isEmpty()) {
        return;   // уже скопирована
    }
    m_customList->addItem(line);
    m_customList->setCurrentRow(m_customList->count() - 1);
    m_customList->editItem(m_customList->currentItem());   // сразу править фразу
}

void SettingsDialog::addCustomCommand()
{
    auto* item = new QListWidgetItem(QStringLiteral("новая фраза = delete-word"));
    m_customList->addItem(item);
    m_customList->setCurrentItem(item);
    m_customList->editItem(item);
}

void SettingsDialog::removeCustomCommand()
{
    qDeleteAll(m_customList->selectedItems());
}

// ---------------------------------------------------------------------------
// Захват клавиши хоткея
// ---------------------------------------------------------------------------

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
