# CLI-стенд: прогоняет WAV через боевой путь ConfigManager -> VoicePipeline
# (SileroVad -> IRecognizer -> TextPostProcessor) без UI и микрофона.
# Собирается только если каталог tools/ существует (см. корневой CMakeLists.txt).

add_executable(vad-asr-test
    vad_asr_test.cpp

    ${CMAKE_SOURCE_DIR}/src/vad/SileroVad.cpp
    ${CMAKE_SOURCE_DIR}/src/asr/TransducerRecognizer.cpp
    ${CMAKE_SOURCE_DIR}/src/asr/NemoCtcRecognizer.cpp
    ${CMAKE_SOURCE_DIR}/src/asr/WhisperRecognizer.cpp
    ${CMAKE_SOURCE_DIR}/src/asr/RecognizerFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/VoicePipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/text/TextPostProcessor.cpp
    # цель вывода «заметки» (--notes): тот же FileInjector, что и в приложении.
    # Зависит только от QtCore, поэтому список библиотек ниже менять не нужно.
    ${CMAKE_SOURCE_DIR}/src/output/FileInjector.cpp

    # header-only / Q_OBJECT заголовки: без них в списке источников AUTOMOC
    # не сгенерирует moc_*.cpp и линковка упадёт на vtable
    ${CMAKE_SOURCE_DIR}/src/vad/IVad.h
    ${CMAKE_SOURCE_DIR}/src/asr/IRecognizer.h
    ${CMAKE_SOURCE_DIR}/src/core/VoicePipeline.h
)

target_include_directories(vad-asr-test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SHERPA_ONNX_INCLUDE_DIR}
)

target_link_libraries(vad-asr-test PRIVATE
    Qt5::Core
    ${SHERPA_ONNX_CXX_LIBRARY}
    ${SHERPA_ONNX_C_LIBRARY}
)

# e2e-стенд привязки вывода к окну: тестовый Qt-пробник (см. tools/e2e).
# Запуск сценариев — tools/e2e/run-e2e.sh (нужны Xvfb, openbox, xdotool, xclip).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/CMakeLists.txt")
    add_subdirectory(e2e)
endif()

# vad-asr-test тоже ставится в пакет: тестировщикам удобно прогонять записи
# микрофона всеми профилями без сборки из исходников.
include(GNUInstallDirs)
install(TARGETS vad-asr-test RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
set_target_properties(vad-asr-test PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/voice-assistant")
