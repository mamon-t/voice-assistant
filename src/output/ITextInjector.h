#pragma once

#include <QString>

class ITextInjector {
public:
    virtual ~ITextInjector() = default;
    
    virtual bool initialize() = 0;
    virtual bool typeText(const QString& text) = 0;
    virtual bool sendKey(const QString& key) = 0;  // "BackSpace", "Return", "Left" и т.д.
    virtual bool isAvailable() const = 0;
};