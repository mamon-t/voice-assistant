#pragma once

#include "output/ITextInjector.h"

#include <QDate>
#include <QDateTime>
#include <QString>

// Вывод распознанного текста в файл — голосовые заметки.
//
// Зачем это нужно. Диктовка в активное окно конфликтует с ручной печатью:
//   * текст уходит в то окно, которое в фокусе В МОМЕНТ ПРИХОДА результата,
//     а это через 0.5–2 с после произнесённой фразы — можно не успеть
//     переключиться обратно;
//   * способ «буфер обмена + Ctrl+V» затирает буфер (прежний текст
//     возвращается через clipboard_restore_ms, но своё копирование в этом
//     окне теряется);
//   * напечатанное и продиктованное перемешиваются в одной строке.
// Файл не зависит ни от фокуса, ни от клавиатуры, ни от буфера обмена:
// заметки диктуются, пока руки продолжают печатать в редакторе.
//
// Формат по умолчанию — один файл на день, <dir>/YYYY-MM-DD.md, каждая запись
// отдельной строкой с меткой времени:
//
//     # Заметки 2026-10-03
//
//     - 14:03:22 — Проверить, как ведёт себя AGC на тихом микрофоне
//     - 14:07:51 — В gazeta.ru сегменты режутся на «Профессиональный боксёр»
//
// Реализует ITextInjector, поэтому переключение цели вывода не меняет
// остальной тракт: TextPostProcessor, пунктуация и орфография работают как
// при обычной диктовке.
class FileInjector : public ITextInjector {
public:
    struct Options {
        QString dir;                 // каталог заметок (создаётся при необходимости)
        QString file;                // явный файл; если задан, dir игнорируется
        QString timestampFormat = QStringLiteral("HH:mm:ss");  // пусто = без метки
        bool    markdown   = true;   // true = "- " в начале строки и .md в имени
        bool    dayHeader  = true;   // заголовок "# Заметки YYYY-MM-DD" в новом файле
    };

    // Два конструктора вместо `= Options()`: дефолтный аргумент того же класса
    // GCC не переваривает (грабля №10 в development-log.md —
    // «default member initializer required before the end of its enclosing class»).
    FileInjector();
    explicit FileInjector(const Options& options);
    ~FileInjector() override;

    bool    initialize() override;
    bool    typeText(const QString& text) override;
    bool    sendKey(const QString& key) override;
    bool    isAvailable() const override;
    QString backendName() const override;

    // Текущий файл: пересчитывается от сегодняшней даты, поэтому запись,
    // начатая вчера, продолжится уже в новом файле.
    QString filePath() const;
    QString lastError() const { return m_lastError; }
    qint64  entries() const { return m_entries; }

    // --- чистые функции, покрыты тестами (диск не трогают) ---

    static QString dailyFileName(const QDate& date, bool markdown = true);

    // Одна строка заметки: марка, метка времени и сам текст
    static QString formatEntry(const QString& text,
                               const QString& timestampFormat,
                               bool markdown,
                               const QDateTime& when = QDateTime::currentDateTime());

    // Стирает последнее слово в строке — для команды «удали слово».
    // Хвостовые пробелы снимаются, маркер списка ("- ") сохраняется.
    static QString eraseLastWord(const QString& line);

private:
    bool    appendLine(const QString& line);
    bool    rewriteLastLine(const QString& newLine);
    QString lastLine() const;
    bool    ensureDir();

    Options m_options;
    bool    m_ready = false;
    QString m_lastError;
    qint64  m_entries = 0;
};
