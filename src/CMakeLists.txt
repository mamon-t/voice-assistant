file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.cpp")

# ОБЯЗАТЕЛЬНО: заголовки тоже должны попасть в список источников.
# IVad.h / IRecognizer.h / IAudioCapture.h и т.п. — header-only классы с Q_OBJECT
# и сигналами. Если AUTOMOC не видит их в составе цели, он не создаёт moc_IVad.cpp,
# и на линковке падает:
#   undefined reference to `IVad::speechStarted()' / `vtable for IVad'
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "*.h")

if(EXISTS "${CMAKE_SOURCE_DIR}/resources/resources.qrc")
    set(RESOURCES "${CMAKE_SOURCE_DIR}/resources/resources.qrc")
else()
    set(RESOURCES "")
endif()

add_executable(voice-assistant
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

target_include_directories(voice-assistant PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SHERPA_ONNX_INCLUDE_DIR}
)

# ONNX Runtime здесь не линкуем намеренно — он уже внутри libsherpa-onnx-c-api.so
target_link_libraries(voice-assistant PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::DBus
    ${SHERPA_ONNX_CXX_LIBRARY}
    ${SHERPA_ONNX_C_LIBRARY}
)
