# tests/CMakeLists.txt
find_package(Qt5 REQUIRED COMPONENTS Test)

add_executable(test_command_parser test_command_parser.cpp)
target_link_libraries(test_command_parser PRIVATE voice-assistant-core Qt5::Test)
add_test(NAME CommandParserTest COMMAND test_command_parser)