cmake_minimum_required(VERSION 3.16)
project(agc_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Core)

add_executable(agc_test
    main.cpp
    ../../src/audio/Agc.cpp
)

target_include_directories(agc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(agc_test PRIVATE Qt5::Core)