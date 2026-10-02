#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <optional>

#include "core/Command.h"

class CommandDictionary;

// Разбирает распознанный текст и решает, команда это или обычный текст.
//
// Принцип: командой считается только фраза, которая ЦЕЛИКОМ совпала со словарём.
// «привет мир» -> std::nullopt (это текст для вставки),
// «удали слово» -> Command::deleteWord().
// «привет точка» -> std::nullopt, потому что совпадения целиком нет; слово «точка»
// внутри фразы обработает TextPostProcessor.
class CommandParser : public QObject {
    Q_OBJECT

public:
    explicit CommandParser(QObject* parent = nullptr);
    ~CommandParser();

    std::optional<Command> parse(const QString& text);

    // Словарь создаётся в конструкторе и уже наполнен стандартными командами;
    // сюда можно добавить свои или дочитать файл.
    CommandDictionary* dictionary();
    const CommandDictionary* dictionary() const;

    // Приведение к виду, в котором хранятся ключи словаря:
    // нижний регистр, без знаков препинания, одиночные пробелы.
    static QString normalize(const QString& text);

private:
    std::unique_ptr<CommandDictionary> m_dictionary;
};
