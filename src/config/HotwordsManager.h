#pragma once

#include <QObject>
#include <QStringList>

class HotwordsManager : public QObject {
    Q_OBJECT

public:
    explicit HotwordsManager(QObject* parent = nullptr);
    ~HotwordsManager();

    QStringList commandsHotwords() const;
    QStringList userHotwords() const;
    QStringList allHotwords() const;

    void reload();

signals:
    void hotwordsChanged();

private:
    QStringList m_commandsHotwords;
    QStringList m_userHotwords;
};