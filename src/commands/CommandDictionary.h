#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <optional>

#include "core/Command.h"

// Словарь диктантных команд: фраза (в нормализованном виде) -> Command.
//
// Фразы намеренно хранятся в нижнем регистре и без знаков препинания —
// CommandParser::normalize() приводит вход к тому же виду, поэтому команды
// распознаются одинаково и из zipformer (строчные, без пунктуации),
// и из Whisper («Удали слово.»).
class CommandDictionary : public QObject {
    Q_OBJECT

public:
    explicit CommandDictionary(QObject* parent = nullptr);
    ~CommandDictionary();

    // Стандартный набор русских команд (см. loadDefaults() в .cpp)
    void loadDefaults();

    // Дочитать команды из файла. Формат строк:
    //   фраза = тип[:аргумент]
    //   удали слово = delete-word
    //   режим редактирования = set-mode:edit
    //   точка = punctuation:.
    // Пустые строки и строки, начинающиеся с '#', игнорируются.
    // Возвращает число добавленных команд.
    int loadFromFile(const QString& path);

    void addCommand(const QString& phrase, Command command);
    void clear();

    // Поиск по уже нормализованной фразе.
    std::optional<Command> find(const QString& normalizedPhrase) const;

    bool contains(const QString& normalizedPhrase) const;
    QStringList phrases() const;   // все фразы — их стоит отдавать в hotwords
    int size() const;

private:
    QMap<QString, Command> m_commands;
};
