#include "HotwordsEditor.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <QFile>
#include <QTextStream>
#include <QDebug>

HotwordsEditor::HotwordsEditor(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Редактор подсказок (Hotwords)");
    resize(350, 400);

    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/voice-assistant/settings.ini";
    m_settings = new QSettings(configPath, QSettings::IniFormat, this);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    m_listWidget = new QListWidget();
    mainLayout->addWidget(m_listWidget);

    QHBoxLayout* inputLayout = new QHBoxLayout();
    m_inputEdit = new QLineEdit();
    m_inputEdit->setPlaceholderText("Новое слово или фраза...");
    m_inputEdit->returnPressed(); // Чтобы Enter не закрывал диалог, а добавлял (обработаем ниже)
    
    m_addBtn = new QPushButton("Добавить");
    m_removeBtn = new QPushButton("Удалить");
    
    connect(m_addBtn, &QPushButton::clicked, this, &HotwordsEditor::addWord);
    connect(m_removeBtn, &QPushButton::clicked, this, &HotwordsEditor::removeWord);
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &HotwordsEditor::addWord);

    inputLayout->addWidget(m_inputEdit);
    inputLayout->addWidget(m_addBtn);
    inputLayout->addWidget(m_removeBtn);
    mainLayout->addLayout(inputLayout);

    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_saveBtn = new QPushButton("Сохранить");
    m_cancelBtn = new QPushButton("Отмена");
    
    connect(m_saveBtn, &QPushButton::clicked, this, &HotwordsEditor::saveHotwords);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);
    mainLayout->addLayout(btnLayout);

    loadHotwords();
}

HotwordsEditor::~HotwordsEditor() = default;

void HotwordsEditor::loadHotwords() {
    m_listWidget->clear();
    QString hwFile = m_settings->value("hotwords/file", "").toString();
    if (hwFile.isEmpty()) {
        hwFile = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/voice-assistant/hotwords.txt";
    }

    QFile file(hwFile);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (!line.isEmpty() && !line.startsWith("#")) {
                m_listWidget->addItem(line);
            }
        }
        file.close();
    }
}

void HotwordsEditor::saveHotwords() {
    QString hwFile = m_settings->value("hotwords/file", "").toString();
    if (hwFile.isEmpty()) {
        hwFile = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/voice-assistant/hotwords.txt";
        m_settings->setValue("hotwords/file", hwFile);
    }

    QFile file(hwFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "# Hotwords for voice assistant\n";
        for (int i = 0; i < m_listWidget->count(); ++i) {
            out << m_listWidget->item(i)->text() << "\n";
        }
        file.close();
    }
    
    emit hotwordsUpdated();
    accept();
}

void HotwordsEditor::addWord() {
    QString word = m_inputEdit->text().trimmed();
    if (!word.isEmpty()) {
        // Проверка на дубликаты
        auto items = m_listWidget->findItems(word, Qt::MatchExactly);
        if (items.isEmpty()) {
            m_listWidget->addItem(word);
        }
        m_inputEdit->clear();
    }
}

void HotwordsEditor::removeWord() {
    qDeleteAll(m_listWidget->selectedItems());
}