// tests/test_command_parser.cpp
#include <QtTest/QtTest>
#include "commands/CommandParser.h"

class TestCommandParser : public QObject {
    Q_OBJECT
    
private slots:
    void testParseModeCommand() {
        CommandParser parser;
        auto cmd = parser.parse("режим редактирования");
        QVERIFY(cmd.has_value());
        QCOMPARE(cmd->type, Command::Type::SetMode);
        QCOMPARE(cmd->mode, Mode::Edit);
    }
    
    void testParseDeleteCommand() {
        CommandParser parser;
        auto cmd = parser.parse("удали слово");
        QVERIFY(cmd.has_value());
        QCOMPARE(cmd->type, Command::Type::DeleteWord);
    }
    
    void testParseNotACommand() {
        CommandParser parser;
        auto cmd = parser.parse("привет мир");
        QVERIFY(!cmd.has_value());
    }
};

QTEST_MAIN(TestCommandParser)
#include "test_command_parser.moc"