cmake_minimum_required(VERSION 3.16)
project(voice-assistant VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 5.13 REQUIRED COMPONENTS Core Widgets Multimedia DBus)

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
