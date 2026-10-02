# Диагностика

## Быстрая таблица

| Симптом | Причина | Лечение |
|---|---|---|
| Segfault в `SileroVad::initialize()`, в стеке `SherpaOnnxCreateVoiceActivityDetector` → `strlen` | рассинхрон ABI libstdc++ | [раздел 1](#1-segfault-при-создании-vad-или-asr) |
| `undefined reference to vtable for IVad` / `IVad::speechStarted()` | заголовки не попали в AUTOMOC | [раздел 2](#2-undefined-reference-to-vtable) |
| `has no member named 'DecodeStream'`, `invalid use of incomplete type 'OfflineStream'` | в дереве остался старый `SenseVoiceRecognizer.cpp` | [раздел 3](#3-ошибки-про-decodestream-и-offlinestream) |
| `sherpa-onnx not found` при `cmake` | библиотека/заголовки не в стандартных путях | [раздел 4](#4-cmake-не-находит-sherpa-onnx) |
| `error while loading shared libraries: libsherpa-onnx-cxx-api.so` | загрузчик не знает про `/usr/local/lib` | [раздел 5](#5-библиотеки-не-находятся-при-запуске) |
| Русский распознаётся как латинская каша | выбрана модель без русского | [раздел 6](#6-русский-распознаётся-неразборчиво) |
| Вместо «привет» печатается «ghbdtn» | `xdotool type` при английской раскладке | [раздел 7](#7-вместо-кириллицы-печатаются-латинские-буквы) |
| Текст не печатается вообще | нет xdotool / Wayland | [раздел 8](#8-текст-не-вставляется) |
| Нет сегментов, `Segments: 0` | не та частота, тишина, высокий порог VAD | [раздел 9](#9-сегменты-не-появляются) |
| Подсказки не действуют | не transducer или не `modified_beam_search` | [раздел 10](#10-hotwords-не-работают) |
| `Each line in vocab should contain two items` | вместо словаря ssentencepiece подсунули `bpe.model` | [раздел 11](#11-ошибка-про-две-колонки-в-vocab) |
| Кириллица в своём конфиге читается кракозябрами | `QTextStream << "строка"` узким литералом | [раздел 12](#12-кракозябры-в-своих-файлах-команд) |

---

## 1. Segfault при создании VAD или ASR

**Симптом.** Компилируется без ошибок, при первом же `initialize()` падает:

```
#0  vpcmpeqb (%rdi),%ymm16,%k0      # strlen в libc, rdi = 0x3f0000003f000000
#1  SherpaOnnxCreateVoiceActivityDetector ()
#2  sherpa_onnx::cxx::VoiceActivityDetector::Create ()
```

**Причина.** Предсобранные sherpa-onnx собраны со старым ABI libstdc++
(`_GLIBCXX_USE_CXX11_ABI=0`), а проект — с новым. `sizeof(VadModelConfig)`
160 против 88, библиотека читает указатель там, где у нас float.

**Лечение.**

```bash
nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs
# >0 — старый ABI:
cmake .. -DSHERPA_ONNX_OLD_CXX_ABI=ON && make -j4
```

Кардинальное решение — собрать sherpa-onnx из исходников тем же компилятором.
Подробности и доказательства — в [sherpa-onnx-notes.md](sherpa-onnx-notes.md).

## 2. Undefined reference to vtable

**Симптом.**

```
undefined reference to `IVad::speechStarted()'
undefined reference to `vtable for IVad'
```

**Причина.** `IVad` / `IRecognizer` / `IAudioCapture` — header-only классы
с `Q_OBJECT`. AUTOMOC обрабатывает только те заголовки, которые перечислены
в источниках цели либо имеют одноимённый `.cpp`.

**Лечение.** В `src/CMakeLists.txt` и `tools/CMakeLists.txt` заголовки уже
добавлены:

```cmake
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "*.h")
add_library(voice-assistant-core STATIC ${SOURCES} ${HEADERS})
```

Если добавляете новый таргет — не забудьте перечислить в нём `IVad.h`
и `IRecognizer.h`.

Альтернатива — завести `IVad.cpp` с одним `#include "vad/IVad.h"` (в проекте
такие файлы уже есть: `src/vad/IVad.cpp`, `src/asr/IRecognizer.cpp`).

## 3. Ошибки про DecodeStream и OfflineStream

**Симптом.**

```
error: 'class sherpa_onnx::cxx::OfflineRecognizer' has no member named 'DecodeStream'
error: invalid use of incomplete type 'class sherpa_onnx::OfflineStream'
error: no matching function for call to 'OfflineRecognizer::OfflineRecognizer(OfflineRecognizerConfig&)'
```

**Причина.** В дереве остался старый `src/asr/SenseVoiceRecognizer.cpp`,
написанный по несуществующему API. Модель SenseVoice не поддерживает русский
и из проекта удалена.

**Лечение.**

```bash
rm -f src/asr/SenseVoiceRecognizer.h src/asr/SenseVoiceRecognizer.cpp
```

Правильный API — в [sherpa-onnx-notes.md](sherpa-onnx-notes.md).

## 4. CMake не находит sherpa-onnx

**Симптом.** `FATAL_ERROR sherpa-onnx не найден`.

**Проверка.**

```bash
ls /usr/local/include/sherpa-onnx/c-api/cxx-api.h
ls /usr/local/lib/libsherpa-onnx-c{,xx}-api.so
```

**Лечение.** Если стоит в другом месте, подскажите пути напрямую:

```bash
cmake .. -DSHERPA_ONNX_INCLUDE_DIR=/opt/sherpa/include \
         -DSHERPA_ONNX_CXX_LIBRARY=/opt/sherpa/lib/libsherpa-onnx-cxx-api.so \
         -DSHERPA_ONNX_C_LIBRARY=/opt/sherpa/lib/libsherpa-onnx-c-api.so
```

## 5. Библиотеки не находятся при запуске

**Симптом.** `error while loading shared libraries: libsherpa-onnx-cxx-api.so`.

**Лечение.** В корневом `CMakeLists.txt` уже прописан rpath на `/usr/local/lib`.
Если не помогло:

```bash
sudo ldconfig
ldd ./src/voice-assistant | grep sherpa     # убедиться, что всё нашлось
```

## 6. Русский распознаётся неразборчиво

**Симптом.** Вместо русского текста — латинские слоги, язык определяется как
`<|yue|>` или `<|en|>`.

**Причина.** Модель не поддерживает русский. SenseVoice обучена на
zh/en/ja/ko/yue; Whisper-base русский знает плохо.

**Лечение.** Проверьте активный профиль:

```bash
grep -A1 '^\[asr\]' ~/.config/voice-assistant/settings.ini
./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
```

Должно быть `active=zipformer-ru` (или `gigaam-v3`). Сравнение моделей —
в [models.md](models.md).

## 7. Вместо кириллицы печатаются латинские буквы

**Симптом.** Продиктовали «привет» — в приложении «ghbdtn».

**Причина.** `xdotool type` эмулирует нажатия клавиш, поэтому результат зависит
от активной раскладки.

**Лечение.** Поставьте утилиту буфера обмена — инжектор перейдёт на вставку
через `Ctrl+V`, которая от раскладки не зависит:

```bash
sudo apt install xclip        # или xsel; на Wayland — wl-copy
```

Проверить, чем сейчас печатаем: строка `XdotoolInjector: … режим=…` в логе при
старте. Принудительный выбор — `[output] method=clipboard|xdotool|auto`.

## 8. Текст не вставляется

**Проверка по шагам.**

```bash
which xdotool || sudo apt install xdotool
VOICE_ASSISTANT_DRYRUN=1 ./src/voice-assistant   # в логе должно быть [dry-run] typeText: …
xdotool type "тест"                              # работает ли вообще
echo $XDG_SESSION_TYPE                           # x11 или wayland
```

На Wayland `xdotool` не работает — нужен `ydotool` (демон + группа `input`);
под него в проекте пока нет реализации `ITextInjector`, см. дорожную карту.

В логе приложения ищите строку `Ввод текста недоступен` — она приходит из
`ApplicationController`, если `initialize()` инжектора не прошёл.

## 9. Сегменты не появляются

**Симптом.** `Segments: 0`, в логе нет `[VAD] speech started`.

**Что проверить.**

1. Формат: нужен строго 16 кГц, моно, int16. При другой частоте `SileroVad`
   шлёт `errorOccurred` с текстом `SileroVad: need 16000 Hz mono int16, got N Hz`.
2. Уровень: включите `[audio] debug_log=true` и посмотрите RMS — должно быть
   больше −35 dB (порог шумового гейта AGC).
3. Порог VAD: `[vad] threshold=0.3` — чувствительнее.
4. Паузы: `[vad] min_silence_duration=0.3` — сегмент закроется быстрее.
5. Файл: `./tools/vad-asr-test <тестовый.wav модели>` — на заведомо рабочем
   WAV. Если там сегменты есть, дело в микрофоне или настройках.

Хвост последней фразы не теряется, если при остановке вызывается `flush()` —
в `ApplicationController::stopRecording()` он есть.

## 10. Hotwords не работают

Подсказки поддерживаются **только** transducer-моделями и только в
`modified_beam_search`. Проверьте профиль:

```bash
grep -E 'engine|decoding_method|bpe_vocab' ~/.config/voice-assistant/settings.ini
```

* `engine=whisper` или `nemo-ctc` → подсказки игнорируются, в логе будет
  соответствующее предупреждение;
* `decoding_method=greedy_search` → контекстный граф не строится;
* `engine=transducer` + BPE-модель без `bpe_vocab` → подсказки словами не
  закодируются, нужен словарь (см. следующий раздел).

В логе ищите `Transducer hotwords: N шт., score=…` — это значит, что список
дошёл до распознавателя.

## 11. Ошибка про две колонки в vocab

**Симптом.**

```
Each line in vocab should contain two items (seperate by space), the first one is bpe token …
```

**Причина.** В `bpe_vocab` указан бинарный `bpe.model` вместо текстового
словаря ssentencepiece.

**Лечение.**

```bash
python3 -m pip install --user sentencepiece
python3 tools/gen_bpe_vocab.py <каталог модели>/bpe.model <каталог модели>/bpe.vocab
```

и прописать в профиле `bpe_vocab=…/bpe.vocab`.

## 12. Кракозябры в своих файлах команд

**Симптом.** `~/.config/voice-assistant/commands.txt` с русскими фразами
не работает, хотя файл выглядит нормально.

**Причина.** Файл сохранён не в UTF-8, либо записывался через
`QTextStream << "кириллица"` узким литералом: такой вызов трактует байты как
Latin-1 и даёт двойную кодировку.

**Лечение.**

```bash
file ~/.config/voice-assistant/commands.txt        # должно быть UTF-8 Unicode text
iconv -f cp1251 -t utf-8 commands.txt -o commands.utf8.txt && mv commands.utf8.txt commands.txt
```

В коде всегда используйте `QStringLiteral("…")` или `QString::fromUtf8(…)`.

## Мелочи, которые не являются ошибками

* `QStandardPaths: XDG_RUNTIME_DIR not set` — нормально при запуске из
  контейнера или по ssh без сессии.
* `QObject::connect: No such signal QPlatformNativeInterface::systemTrayWindowChanged`
  — предупреждение Qt о системном лотке, на работу не влияет.
* `Creating a resampler: in_sample_rate: 24000, output_sample_rate: 16000` в выводе
  sherpa-onnx — информационное: стрим сам ресемплит вход.
* `AutoGen warning: "…AspellChecker.cpp" is empty` — файл действительно пустой,
  см. дорожную карту в README.
