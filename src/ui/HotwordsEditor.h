#pragma once

#include <QDialog>

class QListWidget;
class QLineEdit;
class QPushButton;

// Редактор пользовательских подсказок (hotwords) — слов и фраз из
// ~/.config/voice-assistant/user_hotwords.txt, которые повышают точность
// распознавания (для transducer-моделей — настоящий контекстный бустинг).
//
// Файл и его формат — те же, что использует ConfigManager/HotwordsManager:
// чтение и запись идут через ConfigManager::loadHotwords()/saveHotwords()
// (UTF-8; грабля №6 — QTextStream без setCodec писал бы кириллицу в Latin-1).
//
// После «Сохранить» испускает hotwordsUpdated(); TrayIcon дёргает
// ApplicationController::reloadHotwords() — подсказки применяются без
// перезапуска.
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
    QListWidget* m_listWidget;
    QLineEdit*   m_inputEdit;
    QPushButton* m_addBtn;
    QPushButton* m_removeBtn;
    QPushButton* m_saveBtn;
    QPushButton* m_cancelBtn;
    QString      m_filePath;    // user_hotwords.txt (из ConfigManager)
};
