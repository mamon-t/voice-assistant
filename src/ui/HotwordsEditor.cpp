#include "HotwordsEditor.h"

#include "config/ConfigManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
HotwordsEditor::HotwordsEditor(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Редактор подсказок (Hotwords)"));
    resize(380, 440);

    // Путь и чтение/запись — через ConfigManager: тот же файл, что грузит
    // HotwordsManager (user_hotwords.txt), тот же UTF-8. Прежняя версия
    // писала в несуществующий ключ hotwords/file и в СВОЙ hotwords.txt,
    // который распознаватель никогда не читал.
    const ConfigManager cfg;
    m_filePath = cfg.userHotwordsPath();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(new QLabel(
        QStringLiteral("Файл: %1\nСлово или фраза в строке; чем точнее формулировка,\n"
                       "тем лучше бустинг. Слишком высокий скор — фантомные вставки.")
            .arg(m_filePath)));

    m_listWidget = new QListWidget();
    // Двойной клик — правка слова на месте (опечатки в подсказках стоят
    // точности распознавания, а пересоздавать слово ради одной буквы утомительно).
    m_listWidget->setEditTriggers(QAbstractItemView::DoubleClicked
                                  | QAbstractItemView::EditKeyPressed);
    mainLayout->addWidget(m_listWidget);

    m_countLabel = new QLabel();
    mainLayout->addWidget(m_countLabel);
    connect(m_listWidget, &QListWidget::itemChanged,
            this, [this]() { updateCount(); });
    connect(m_listWidget->model(), &QAbstractItemModel::rowsInserted,
            this, [this]() { updateCount(); });
    connect(m_listWidget->model(), &QAbstractItemModel::rowsRemoved,
            this, [this]() { updateCount(); });

    auto* inputLayout = new QHBoxLayout();
    m_inputEdit = new QLineEdit();
    m_inputEdit->setPlaceholderText(QStringLiteral("Новое слово или фраза..."));
    // Enter в поле ввода добавляет слово (connect ниже), а не закрывает диалог.

    m_addBtn    = new QPushButton(QStringLiteral("Добавить"));
    m_removeBtn = new QPushButton(QStringLiteral("Удалить"));

    connect(m_addBtn,    &QPushButton::clicked,      this, &HotwordsEditor::addWord);
    connect(m_removeBtn, &QPushButton::clicked,      this, &HotwordsEditor::removeWord);
    connect(m_inputEdit, &QLineEdit::returnPressed,  this, &HotwordsEditor::addWord);

    // «Удалить» живёт только когда есть выделение — иначе кнопка обещает
    // действие, которого нет.
    connect(m_listWidget, &QListWidget::itemSelectionChanged, this, [this]() {
        m_removeBtn->setEnabled(!m_listWidget->selectedItems().isEmpty());
    });
    m_removeBtn->setEnabled(false);

    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_addBtn);
    inputLayout->addWidget(m_removeBtn);
    mainLayout->addLayout(inputLayout);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveBtn   = new QPushButton(QStringLiteral("Сохранить"));
    m_cancelBtn = new QPushButton(QStringLiteral("Отмена"));

    connect(m_saveBtn,   &QPushButton::clicked, this, &HotwordsEditor::saveHotwords);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);
    mainLayout->addLayout(btnLayout);

    loadHotwords();
}

HotwordsEditor::~HotwordsEditor() = default;

void HotwordsEditor::loadHotwords()
{
    m_listWidget->clear();
    const ConfigManager cfg;
    m_listWidget->addItems(cfg.loadHotwords(m_filePath));
    updateCount();
}

void HotwordsEditor::updateCount()
{
    m_countLabel->setText(
        QStringLiteral("Слов: %1. Правка — двойным кликом; в силу вступает после «Сохранить».")
            .arg(m_listWidget->count()));
}

void HotwordsEditor::saveHotwords()
{
    QStringList words;
    words.reserve(m_listWidget->count());
    for (int i = 0; i < m_listWidget->count(); ++i) {
        const QString t = m_listWidget->item(i)->text().trimmed();
        if (!t.isEmpty()) {
            words << t;
        }
    }

    const ConfigManager cfg;
    cfg.saveHotwords(m_filePath, words);   // UTF-8, по слову в строке

    emit hotwordsUpdated();
    accept();
}

void HotwordsEditor::addWord()
{
    const QString word = m_inputEdit->text().trimmed();
    if (!word.isEmpty()) {
        if (m_listWidget->findItems(word, Qt::MatchExactly).isEmpty()) {
            m_listWidget->addItem(word);
        }
        m_inputEdit->clear();
    }
}

void HotwordsEditor::removeWord()
{
    qDeleteAll(m_listWidget->selectedItems());
}
