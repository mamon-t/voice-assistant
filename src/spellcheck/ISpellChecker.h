#pragma once

#include <QString>
#include <QStringList>

class ISpellChecker {
public:
    virtual ~ISpellChecker() = default;
    
    virtual bool initialize() = 0;
    virtual QStringList check(const QString& text) = 0;  // возвращает список ошибок
    virtual QStringList suggest(const QString& word) = 0;  // возвращает варианты исправления
};