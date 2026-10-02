#pragma once

#include "output/ITextInjector.h"

class XdotoolInjector : public ITextInjector {
public:
    XdotoolInjector();
    ~XdotoolInjector();

    bool initialize() override;
    bool typeText(const QString& text) override;
    bool sendKey(const QString& key) override;
    bool isAvailable() const override;

private:
    bool m_available = false;
};