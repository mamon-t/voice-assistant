#pragma once

#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

class HotwordsEditor : public QDialog {
    Q_OBJECT

public:
    explicit HotwordsEditor(QWidget* parent = nullptr);
    ~HotwordsEditor();

signals:
    void hotwordsUpdated();

private slots:
    void loadHotwords();
    void saveHotwords();
    void addWord();
    void removeWord();

private:
    QSettings* m_settings;
    QListWidget* m_listWidget;
    QLineEdit* m_inputEdit;
    QPushButton* m_addBtn;
    QPushButton* m_removeBtn;
    QPushButton* m_saveBtn;
    QPushButton* m_cancelBtn;
};