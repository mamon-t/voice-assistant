cmake_minimum_required(VERSION 3.16)
project(voice-assistant VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 5.13 REQUIRED COMPONENTS Core Widgets Multimedia DBus Network)

# ---------------------------------------------------------------------------
# sherpa-onnx: ОДИН источник и для VAD (Silero), и для ASR (zipformer-ru / Whisper).
#
# Отдельный ONNX Runtime искать и линковать НЕ НУЖНО:
#   * заголовок <sherpa-onnx/c-api/cxx-api.h> уже подключает всё необходимое;
#   * libonnxruntime.so связана внутри libsherpa-onnx-c-api.so;
#   * свой Ort::Env/Ort::Session для Silero VAD не пишем — в sherpa-onnx
#     есть VoiceActivityDetector.
# ---------------------------------------------------------------------------
find_path(SHERPA_ONNX_INCLUDE_DIR
    NAMES sherpa-onnx/c-api/cxx-api.h
    PATHS /usr/local/include /usr/include
)

find_library(SHERPA_ONNX_CXX_LIBRARY
    NAMES sherpa-onnx-cxx-api
    PATHS /usr/local/lib /usr/lib
)

find_library(SHERPA_ONNX_C_LIBRARY
    NAMES sherpa-onnx-c-api
    PATHS /usr/local/lib /usr/lib
)

if(NOT SHERPA_ONNX_INCLUDE_DIR OR NOT SHERPA_ONNX_CXX_LIBRARY OR NOT SHERPA_ONNX_C_LIBRARY)
    message(FATAL_ERROR
        "sherpa-onnx не найден. Нужны: sherpa-onnx/c-api/cxx-api.h, "
        "libsherpa-onnx-cxx-api.so, libsherpa-onnx-c-api.so")
endif()

message(STATUS "sherpa-onnx include : ${SHERPA_ONNX_INCLUDE_DIR}")
message(STATUS "sherpa-onnx cxx lib : ${SHERPA_ONNX_CXX_LIBRARY}")
message(STATUS "sherpa-onnx c   lib : ${SHERPA_ONNX_C_LIBRARY}")

# ---------------------------------------------------------------------------
# ABI libstdc++ — ГРАБЛИ, на которые легко наступить.
#
# Официальные ПРЕДСОБРАННЫЕ sherpa-onnx (linux-x64 shared) собраны со старым
# ABI: _GLIBCXX_USE_CXX11_ABI=0. Проверено по манглингу:
#   nm -D --defined-only libsherpa-onnx-cxx-api.so | grep 12CreateStream
#   -> ...17OfflineRecognizer12CreateStreamERKSs   ("Ss" = старый ABI)
#
# Если такая библиотека слинкована с кодом, собранным в стандартном (новом)
# ABI, то cxx::VadModelConfig / OfflineRecognizerConfig передаются через
# границу библиотек с разной раскладкой std::string (32 байта против 8):
# sizeof(VadModelConfig) = 160 вместо 88, библиотека читает ten_vad.model
# из наших float'ов и падает в strlen() -> SIGSEGV внутри Create().
# Компилируется при этом БЕЗ единой ошибки — ловится только в рантайме.
#
# Как проверить СВОЮ библиотеку (одна команда):
#   nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs
#     >0 -> old-ABI  => собирай с -DSHERPA_ONNX_OLD_CXX_ABI=ON
#      0 -> new-ABI  => оставь OFF (обычно так, если sherpa-onnx собиралась
#                      из исходников тем же g++, что и проект)
# ---------------------------------------------------------------------------
option(SHERPA_ONNX_OLD_CXX_ABI
       "Собирать проект с _GLIBCXX_USE_CXX11_ABI=0 (нужно для предсобранных sherpa-onnx)"
       OFF)
if(SHERPA_ONNX_OLD_CXX_ABI)
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
    message(STATUS "Включён старый ABI libstdc++ (_GLIBCXX_USE_CXX11_ABI=0) — "
                   "под предсобранные sherpa-onnx")
endif()

# ---------------------------------------------------------------------------
# ABI-САМОПРОВЕРКА на этапе конфигурации.
#
# Рассинхрон флага и библиотеки не даёт ошибок компиляции: он всплывает либо
# segfault'ом в рантайме (грабля №1), либо «undefined reference to
# ...CreateStream(std::string const&)» при линковке — и то и другое дорого
# диагностировать. Проверяем символы библиотеки тем же способом, что описан
# выше (nm | grep ERKSs), и падаем сразу с готовой командой исправления.
#
# Как читать ошибку ld, если она всё-таки случилась:
#   «std::string const&»                    -> объекты old-ABI (флаг ON),
#                                              а библиотека new-ABI
#   «std::__cxx11::basic_string<...> const&» -> объекты new-ABI (флаг OFF),
#                                              а библиотека old-ABI
# ---------------------------------------------------------------------------
find_program(NM_EXECUTABLE nm)
if(NM_EXECUTABLE AND SHERPA_ONNX_CXX_LIBRARY)
    execute_process(COMMAND ${NM_EXECUTABLE} -D --defined-only ${SHERPA_ONNX_CXX_LIBRARY}
                    OUTPUT_VARIABLE SHERPA_ABI_SYMBOLS ERROR_QUIET)
    string(LENGTH "${SHERPA_ABI_SYMBOLS}" _syms_len)
    if(_syms_len GREATER 0)   # nm отработал — можно верить результату
        string(REGEX MATCHALL "ERKSs" _old_abi_hits "${SHERPA_ABI_SYMBOLS}")
        list(LENGTH _old_abi_hits _old_abi_count)
        if(_old_abi_count EQUAL 0 AND SHERPA_ONNX_OLD_CXX_ABI)
            message(FATAL_ERROR
                "ABI-проверка: ${SHERPA_ONNX_CXX_LIBRARY} собрана с НОВЫМ ABI libstdc++ "
                "(в символах нет 'ERKSs'), а SHERPA_ONNX_OLD_CXX_ABI=ON — линковка упадёт "
                "с «undefined reference to ...CreateStream(std::string const&)».\n"
                "Исправление: rm -rf build (или хотя бы build/CMakeCache.txt и CMakeFiles) "
                "и переконфигурировать БЕЗ флага: cmake ..\n"
                "Так обычно и есть, когда sherpa-onnx собиралась из исходников тем же g++, "
                "что и проект.")
        elseif(_old_abi_count GREATER 0 AND NOT SHERPA_ONNX_OLD_CXX_ABI)
            message(FATAL_ERROR
                "ABI-проверка: ${SHERPA_ONNX_CXX_LIBRARY} собрана со СТАРЫМ ABI libstdc++ "
                "(в символах есть 'ERKSs': ${_old_abi_count} шт.), а "
                "SHERPA_ONNX_OLD_CXX_ABI=OFF — будет segfault в Create() (грабля №1).\n"
                "Исправление: rm -rf build и cmake .. -DSHERPA_ONNX_OLD_CXX_ABI=ON\n"
                "Так обычно и есть с официальными предсобранными sherpa-onnx.")
        else()
            message(STATUS "ABI-проверка sherpa-onnx: OK (old-ABI символов: "
                           "${_old_abi_count}, флаг SHERPA_ONNX_OLD_CXX_ABI="
                           "${SHERPA_ONNX_OLD_CXX_ABI})")
        endif()
    else()
        message(STATUS "ABI-проверка sherpa-onnx: пропущена (nm не вернул символы)")
    endif()
endif()

# Чтобы бинарник запускался и без `sudo ldconfig` (библиотеки стоят в /usr/local/lib)
list(APPEND CMAKE_BUILD_RPATH "/usr/local/lib")
list(APPEND CMAKE_INSTALL_RPATH "/usr/local/lib")

add_subdirectory(src)

if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt")
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Установка и пакеты (.deb / .rpm через CPack).
#
# Карта файлов (важна для ЧИСТОГО сноса — всё системное удаляет dpkg/rpm,
# в домашний каталог пакет не лезет ни при установке, ни при удалении):
#   /usr/bin/voice-assistant, /usr/bin/vad-asr-test
#   /usr/lib/voice-assistant/          — sherpa-onnx + onnxruntime + hunspell
#                                        (RPATH бинарника: $ORIGIN/../lib/voice-assistant)
#   /usr/share/applications/voice-assistant.desktop
#   /usr/share/metainfo/voice-assistant.metainfo.xml
#   /usr/share/icons/hicolor/scalable/apps/voice-assistant.svg
#   /usr/share/voice-assistant/settings.ini.default
#   /usr/share/doc/voice-assistant/    — README, LICENSE, docs/*.md
# Пользовательские данные (пакет их НЕ трогает, в т.ч. при purge — политика
# дистрибутивов; очистка одной командой описана в README):
#   ~/.config/voice-assistant/         — настройки, подсказки, свои команды
#   ~/.local/share/voice-assistant/    — заметки, записи микрофона
#   ~/.voice_models/                   — модели (качает приложение, не пакет)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(FILES dist/voice-assistant.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES dist/voice-assistant.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES resources/icons/mic-on.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME voice-assistant.svg)
install(FILES config/settings.ini
        DESTINATION ${CMAKE_INSTALL_DATADIR}/voice-assistant
        RENAME settings.ini.default)
install(FILES README.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
        FILES_MATCHING PATTERN "*.md")

# ---------------------------------------------------------------------------
# CPack: cpack -G DEB   и/или   cpack -G RPM
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "voice-assistant")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Mikhail Alekseev <2988373@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/mamon-t/voice-assistant")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Offline voice dictation into any application (X11; evdev hotkey works in Wayland/TTY)")
set(CPACK_PACKAGE_DESCRIPTION
    "Голосовой помощник для набора и правки текста диктовкой. Полностью офлайн:
Silero VAD + zipformer-ru / GigaAM / Whisper через sherpa-onnx. Глобальный
хоткей через evdev (X11, Wayland, TTY), вставка в любое окно X11 (буфер обмена
+ Ctrl+V или xdotool, с привязкой к окну начала записи), голосовые команды
правки и пунктуация, проверка правописания hunspell, вторая цель вывода —
файл заметок (диктовка не мешает печатать руками). Модели (~110 МБ)
скачиваются при первом запуске из меню лотка или командой
voice-assistant --download-model zipformer-ru.")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES TRUE)

# --- DEB ---
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
# hunspell намеренно НЕ в зависимостях: libhunspell-1.7.so.0 кладём в
# /usr/lib/voice-assistant (в Ubuntu 24.04 пакет зовётся ...t64 — общая
# сборка под 22.04 не удовлетворила бы зависимость на 22.04+24.04 сразу).
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libc6, libstdc++6, libqt5core5a, libqt5gui5, libqt5widgets5, libqt5multimedia5, libqt5dbus5, libqt5network5, xdotool, xclip, x11-utils")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
    "hunspell-ru, x11-xkb-utils, pulseaudio")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/dist/packaging/postinst")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# --- RPM ---
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Productivity")
set(CPACK_RPM_PACKAGE_REQUIRES
    "qt5-qtbase, qt5-qtmultimedia, xdotool, xclip, xorg-x11-utils")
set(CPACK_RPM_PACKAGE_SUGGESTS "hunspell-ru, x11-xkb-utils")
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
    "${CMAKE_SOURCE_DIR}/dist/packaging/postinst")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
# свои библиотеки в /usr/lib/voice-assistant: не давать rpmbuild ругаться
# на нерезолвимые soname sherpa/onnxruntime (они рядом с бинарником, RPATH)
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none")

include(CPack)
