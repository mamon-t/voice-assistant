#include "config/HotwordsManager.h"

#include "config/ConfigManager.h"

#include <QDebug>

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

void HotwordsManager::setConfigManager(const ConfigManager* config) {
    m_config = config;
}

void HotwordsManager::reload() {
    const ConfigManager cfg;                 // запасной вариант, если setConfigManager не звали
    const ConfigManager* c = m_config ? m_config : &cfg;

    m_commandsHotwords = c->loadHotwords(c->commandsHotwordsPath());
    m_userHotwords     = c->loadHotwords(c->userHotwordsPath());

    qDebug().noquote() << QString("HotwordsManager: командных=%1, пользовательских=%2")
                              .arg(m_commandsHotwords.size())
                              .arg(m_userHotwords.size());

    emit hotwordsChanged();
}