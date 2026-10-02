#include "config/HotwordsManager.h"

HotwordsManager::HotwordsManager(QObject* parent)
    : QObject(parent)
{
}

HotwordsManager::~HotwordsManager() = default;

QStringList HotwordsManager::commandsHotwords() const {
    return m_commandsHotwords;
}

QStringList HotwordsManager::userHotwords() const {
    return m_userHotwords;
}

QStringList HotwordsManager::allHotwords() const {
    return m_commandsHotwords + m_userHotwords;
}

void HotwordsManager::reload() {
    // TODO: перечитать файлы
    emit hotwordsChanged();
}