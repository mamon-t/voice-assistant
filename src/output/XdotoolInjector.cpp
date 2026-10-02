#include "output/XdotoolInjector.h"
#include <QProcess>

XdotoolInjector::XdotoolInjector() = default;
XdotoolInjector::~XdotoolInjector() = default;

bool XdotoolInjector::initialize() {
    m_available = (QProcess::execute("which", {"xdotool"}) == 0);
    return m_available;
}

bool XdotoolInjector::typeText(const QString& text) {
    Q_UNUSED(text)
    // TODO
    return m_available;
}

bool XdotoolInjector::sendKey(const QString& key) {
    Q_UNUSED(key)
    // TODO
    return m_available;
}

bool XdotoolInjector::isAvailable() const {
    return m_available;
}