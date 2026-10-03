#pragma once

#include "spellcheck/ISpellChecker.h"

#include <QString>
#include <QStringList>
#include <memory>

// Проверка правописания на hunspell (тот же движок, что в LibreOffice).
//
// Почему hunspell, а не aspell: русский словарь hunspell-ru (ru_RU.dic, 3.4 МБ)
// поддерживается и знает современную лексику, тогда как aspell-ru не обновлялся
// с середины 2000-х. Замер на словаре из Debian bookworm:
//   "прверка"   -> ошибка, варианты: проверка, поверка, привертка
//   "помошника" -> ошибка, варианты: поморника, помощника, доминошника
//   "привет", "проверка", "расшифруй", "голосового", "помощника" -> OK
//
// Зависимость НЕОБЯЗАТЕЛЬНАЯ: если libhunspell-dev не найден на этапе cmake,
// проект собирается без него (HAVE_HUNSPELL не определён), initialize()
// возвращает false и объясняет, что поставить. См. src/CMakeLists.txt.
//
// Класс не QObject: ISpellChecker — обычный интерфейс, мокать его не нужно.
class HunspellChecker : public ISpellChecker {
public:
    struct Options {
        QString lang = QStringLiteral("ru_RU");   // имя словаря: <lang>.aff + <lang>.dic
        QString dictionaryDir;                    // пусто = искать в стандартных путях
        int     maxSuggestions = 5;
    };

    HunspellChecker();
    explicit HunspellChecker(const Options& options);
    ~HunspellChecker() override;

    bool initialize() override;
    QStringList check(const QString& text) override;    // слова с ошибками, по порядку, без повторов
    QStringList suggest(const QString& word) override;  // варианты исправления

    bool    isAvailable() const { return m_ready; }
    QString dictionaryPath() const { return m_dicPath; }
    QString lastError() const { return m_lastError; }

    // Разбивает текст на слова: только буквы (Unicode), цифры и пунктуация отбрасываются.
    // Вынесено в статику, чтобы её можно было тестировать без словаря.
    static QStringList splitWords(const QString& text);

    // Где искать словарь, если каталог не задан явно.
    static QStringList standardDictionaryDirs();

private:
    Options m_options;
    QString m_dicPath;
    QString m_affPath;
    QString m_lastError;
    bool    m_ready = false;

    struct Impl;                       // прячем hunspell.h из заголовка (и от ABI-зависимости)
    std::unique_ptr<Impl> m_impl;
};
