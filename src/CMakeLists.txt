file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "*.h")

# main.cpp не входит в библиотеку: он нужен только исполняемому файлу,
# а тесты должны линковаться со всем остальным кодом.
set(CORE_SOURCES ${ALL_SOURCES})
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# ---------------------------------------------------------------------------
# Статическая библиотека с ядром. Нужна тестам (tests/CMakeLists.txt ссылается
# на цель voice-assistant-core) и позволяет гонять QTest без GUI и микрофона.
# ---------------------------------------------------------------------------
add_library(voice-assistant-core STATIC
    ${CORE_SOURCES}
    ${HEADERS}   # ОБЯЗАТЕЛЬНО: header-only классы с Q_OBJECT (IVad, IRecognizer,
                 # IAudioCapture) иначе не попадают в AUTOMOC -> undefined
                 # reference to `vtable for IVad'
)

target_include_directories(voice-assistant-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SHERPA_ONNX_INCLUDE_DIR}
)

# ONNX Runtime не линкуем намеренно — он уже внутри libsherpa-onnx-c-api.so
target_link_libraries(voice-assistant-core PUBLIC
    Qt5::Core
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::DBus
    ${SHERPA_ONNX_CXX_LIBRARY}
    ${SHERPA_ONNX_C_LIBRARY}
)

# ---------------------------------------------------------------------------
# Исполняемый файл
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/resources/resources.qrc")
    set(RESOURCES "${CMAKE_SOURCE_DIR}/resources/resources.qrc")
else()
    set(RESOURCES "")
endif()

add_executable(voice-assistant
    main.cpp
    ${RESOURCES}
)

target_link_libraries(voice-assistant PRIVATE voice-assistant-core)
