#pragma once

#include <QObject>
#include <QStringList>

class ConfigManager;

class HotwordsManager : public QObject {
    Q_OBJECT

public:
    explicit HotwordsManager(QObject* parent = nullptr);
    ~HotwordsManager();

    // Откуда брать файлы подсказок. Если не задан — используются пути по умолчанию
    // из ConfigManager (~/.config/voice-assistant/{commands,user}_hotwords.txt).
    void setConfigManager(const ConfigManager* config);

    QStringList commandsHotwords() const;
    QStringList userHotwords() const;
    QStringList allHotwords() const;

    void reload();

signals:
    void hotwordsChanged();

private:
    const ConfigManager* m_config = nullptr;
    QStringList m_commandsHotwords;
    QStringList m_userHotwords;
};