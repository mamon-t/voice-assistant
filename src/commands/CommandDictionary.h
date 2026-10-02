#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include "core/Command.h"

class CommandDictionary : public QObject {
    Q_OBJECT

public:
    explicit CommandDictionary(QObject* parent = nullptr);
    ~CommandDictionary();

    void loadDefaults();
    void addCommand(const QString& phrase, Command command);

private:
    QMap<QString, Command> m_commands;
};