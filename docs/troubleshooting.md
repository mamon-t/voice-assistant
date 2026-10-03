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
| Приложение работает, но речь не распознаётся | не инициализировался ASR: нет файлов модели | [раздел 0](#0-ничего-не-распознаётся-первое-что-проверить) |
| Текст не печатается вообще | нет xdotool / Wayland | [раздел 8](#8-текст-не-вставляется) |
| Буфер обмена затирается при диктовке | вставка идёт через буфер | `[output] preserve_clipboard=true` (по умолчанию включено) |
| `undefined reference to Hunspell::spell(std::string const&…)` | ABI-конфликт libhunspell | [раздел 13](#13-ошибка-линковки-с-hunspell) |
| Режим правописания молчит | нет словаря или проект собран без hunspell | [раздел 14](#14-проверка-правописания-не-работает) |
| Хоткей не срабатывает | нет прав на `/dev/input` или неизвестное имя клавиши | [раздел 15](#15-глобальный-хоткей-не-работает) |
| Распознавание плохое, непонятно кто виноват | нужен снимок тракта | [раздел 16](#16-как-диагностировать-качество-распознавания) |
| Нет сегментов, `Segments: 0` | не та частота, тишина, высокий порог VAD | [раздел 9](#9-сегменты-не-появляются) |
| Подсказки не действуют | не transducer или не `modified_beam_search` | [раздел 10](#10-hotwords-не-работают) |
| `Each line in vocab should contain two items` | вместо словаря ssentencepiece подсунули `bpe.model` | [раздел 11](#11-ошибка-про-две-колонки-в-vocab) |
| Кириллица в своём конфиге читается кракозябрами | `QTextStream << "строка"` узким литералом | [раздел 12](#12-кракозябры-в-своих-файлах-команд) |

---

## 0. Ничего не распознаётся: первое, что проверить

```bash
./src/voice-assistant --check
```

Инструмент печатает все пути из конфига с пометкой `OK (размер)` или `НЕТ ФАЙЛА`
и итоговое число проблем. Самый частый случай — модель не скачана или лежит
в другом каталоге:

```
  [zipformer-ru] * engine=transducer threads=2 decoding=modified_beam_search
  encoder  : /home/…/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/encoder.int8.onnx  НЕТ ФАЙЛА
  ИТОГ: профиль НЕ готов
```

При этом в логе запуска будет строка

```
VoicePipeline: профиль 'zipformer-ru': encoder не найден: …
```

и приложение переведётся в `Mode::Error` (в лотке — иконка `error.svg`, плюс
всплывающее уведомление). Запись при этом не начинается: `startRecording()`
и `setMode()` отказываются работать без инициализированного тракта и говорят об этом.

Лечение — скачать модель (см. README, раздел «Модели») и перепроверить:

```bash
mkdir -p ~/.voice_models && cd ~/.voice_models
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2
tar xf sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2 && rm sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2
./src/voice-assistant --check        # должно стать «всё на месте»
```

Если файлы на месте, но тракт всё равно не поднимается — проверьте ABI
([раздел 1](#1-segfault-при-создании-vad-или-asr)) и прогоните запись напрямую:

```bash
./tools/vad-asr-test ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/test_wavs/0.wav \
    --config ~/.config/voice-assistant/settings.ini
```

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

## 13. Ошибка линковки с hunspell

**Симптом.**

```
undefined reference to `Hunspell::spell(std::string const&, int*, std::string*)'
undefined reference to `Hunspell::suggest(std::string const&)'
```

**Причина.** Тот же конфликт ABI libstdc++, что и с sherpa-onnx: проект собран
с `-DSHERPA_ONNX_OLD_CXX_ABI=ON` (старый ABI), а системный `libhunspell` —
с новым. C++ API hunspell принимает `std::string`, поэтому символы не совпадают.

**Лечение.** Уже сделано в коде: `HunspellChecker` использует **чистый C API**
(`hunspell.h`: `Hunspell_create`, `Hunspell_spell`, `Hunspell_suggest`),
он работает с `char*` и от ABI не зависит. Если ошибка вернулась — значит,
где-то снова подключён `hunspell/hunspell.hxx`.

## 14. Проверка правописания не работает

**Диагностика.**

```bash
./src/voice-assistant --check      # раздел «Правописание»
```

Варианты:

* `выключено ([spellcheck] enabled=false)` — включите в конфиге;
* `Словарь 'ru_RU' не найден` → `sudo apt install hunspell-ru`. Если словарь
  лежит в нестандартном месте — `[spellcheck] dictionary_dir=/путь`;
* `Словарь … в кодировке 'KOI8-R', а нужен UTF-8` → нужен словарь в UTF-8
  (пакет `hunspell-ru` в Debian/Ubuntu именно такой);
* `Проект собран без поддержки hunspell` → `sudo apt install libhunspell-dev`
  и пересобрать: `cd build && cmake .. && make -j4`. В выводе `cmake` должна
  появиться строка `hunspell 1.7.1: найден — проверка правописания включена`.

**Проверка без приложения:**

```bash
cd build && ./tests/test_voice_units spellCheckRealDictionary
```

Если словарь не установлен, тест корректно пропустится (`SKIP`), а не упадёт.

## 15. Глобальный хоткей не работает

**Диагностика.**

```bash
./src/voice-assistant --check        # раздел «Хоткей»
```

* `клавиша : … НЕ РАЗОБРАНА` — неверное имя в `[hotkey] key`. Годятся `F8`,
  `KEY_F13`, `ctrl+space`, `Ctrl+Alt+Shift+F13` или числовой код (`66` = `KEY_F8`;
  полная таблица — `/usr/include/linux/input-event-codes.h`).
* `/dev/input: НЕТ` — ядро без evdev или контейнер; хоткей недоступен в принципе.
* `N устройств event*, доступно для чтения: 0` — нет прав:

```bash
sudo usermod -aG input $USER
# выйти и зайти заново (или: newgrp input — для проверки в текущей оболочке)
ls -l /dev/input/event0
```

* В логе приложения ищите `EvdevHotkey: клавиша 'KEY_F8' … устройств: N` —
  если строка есть, слушатель запущен.
* Клавиша доходит до активного приложения и мешает? Это нормально при `grab=false`:
  evdev только слушает. Возьмите клавишу, которую никто не использует
  (`F13`–`F24`), или комбинацию с модификатором. `grab=true` решает проблему,
  но забирает устройство эксклюзивно — **не включайте его для основной клавиатуры**.
* Совсем без прав на `/dev/input` можно обойтись: повесьте клавишу в настройках
  WM/DE на `dbus-send … org.voiceassistant.App.startRecording`.

**Проверка логики без железа:**

```bash
cd build && ./tests/test_voice_units hotkeyHandlesSyntheticEvents hotkeyRequiresModifiers
```

Тесты скармливают слушателю синтетические `struct input_event`, поэтому
нажатие, отпускание, автоповтор (`value=2`) и модификаторы проверяются
без единого реального устройства.

## 16. Как диагностировать качество распознавания

Симптом: текст распознаётся с ошибками, и непонятно, кто виноват — микрофон,
AGC, VAD или модель. Порядок действий, который разделяет эти слои.

**Шаг 1. Снимок сырого тракта (без AGC, без GUI):**

```bash
./src/voice-assistant --record 20 ~/mic-raw.wav
```

Прочитайте тот же текст, что и при обычной диктовке. Файл — ровно то,
что отдаёт микрофон.

**Шаг 2. Прогон снимка всеми профилями:**

```bash
for p in zipformer-ru gigaam-v3 gigaam-v3-ctc whisper-base; do
    echo "===== $p ====="
    ./tools/vad-asr-test ~/mic-raw.wav --config ~/.config/voice-assistant/settings.ini --profile $p
done
```

Инструмент печатает и `raw` (что выдал ASR), и `text` (что ушло бы в приложение),
плюс длительности сегментов и RTF.

**Шаг 3. Сравните с живой диктовкой.**

* На файле заметно лучше, чем вживую → виноват аудиотракт в реальном времени:
  включите `[audio] debug_log=true` и посмотрите RMS/gain в момент речи.
  Gain, прижатый к максимуму (10), или срабатывающий шумовой гейт (-35 dB)
  сразу видны. Крутите `Agc` в конструкторе `ApplicationController`.
* На файле так же плохо → виновата модель. Сравните профили из шага 2:
  обычно `gigaam-v3` тянет шум и быструю речь лучше `zipformer-ru small`.
* Сегменты слишком длинные (10+ с) → VAD не находит пауз: уменьшите
  `[vad] min_silence_duration` до 0.3 и/или поднимите `threshold`.
* Сегменты режут слова пополам → наоборот, увеличьте `min_silence_duration`.

**Шаг 4. Снимок после AGC** — то, что реально слышит ASR:
пункт «Проверить микрофон (запись в WAV)» в трее, или D-Bus:

```bash
BUS=org.voiceassistant.App; OBJ=/org/voiceassistant/App
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.startMicCheck
# ... говорим ...
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.stopMicCheck
```

Источник выбирается в `[audio] mic_check_source`: `agc` (по умолчанию) или `raw`.
Сравнив две записи одного и того же текста, видно, помогает AGC или вредит.

Проверить, что сам WAV в порядке, можно любым проигрывателем или так:

```bash
python3 -c "import wave,sys; w=wave.open(sys.argv[1]); print(w.getparams(), w.getnframes()/w.getframerate(),'с')" ~/mic-raw.wav
```
