#pragma once

#include "spellcheck/ISpellChecker.h"

class AspellChecker : public ISpellChecker {
public:
    AspellChecker();
    ~AspellChecker();

    bool initialize() override;
    QStringList check(const QString& text) override;
    QStringList suggest(const QString& word) override;

private:
    bool m_available = false;
};