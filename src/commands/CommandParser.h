#pragma once

#include <QObject>
#include <QString>
#include <optional>
#include "core/Command.h"

class CommandParser : public QObject {
    Q_OBJECT

public:
    explicit CommandParser(QObject* parent = nullptr);
    ~CommandParser();

    std::optional<Command> parse(const QString& text);
};