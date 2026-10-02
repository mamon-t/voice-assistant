# voice-assistant

Офлайновый голосовой помощник для Linux: диктовка текста в любое поле ввода,
голосовые команды правки и проверка правописания. Распознавание работает локально —
без интернета, без облаков и без отправки аудио куда-либо. Рассчитан на слабое
железо (Celeron, 4–16 ГБ ОЗУ).

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)
![Qt 5.13+](https://img.shields.io/badge/Qt-5.13%2B-41CD52.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![Status](https://img.shields.io/badge/status-alpha-orange.svg)

---

## Возможности

* **Диктовка в любое приложение** — текст вставляется через буфер обмена + `Ctrl+V`
  либо через `xdotool type`, работает в браузере, редакторах и терминале.
* **Полностью офлайн** — VAD (Silero) и ASR (zipformer-ru / GigaAM / Whisper)
  выполняются локально через [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx).
* **Голосовые команды** — смена режима, удаление слова/строки, новая строка,
  знаки препинания. Словарь расширяется своим файлом.
* **Пунктуация голосом** — «точка», «запятая», «вопросительный знак», «абзац»
  превращаются в знаки; заглавные буквы и точка в конце расставляются автоматически.
* **Подсказки пользователя (hotwords)** — редкие слова, имена и термины из файла
  повышают точность распознавания: для transducer-моделей работает настоящий
  контекстный бустинг, а не замена по тексту.
* **Переключаемые профили ASR** — модель меняется одной строкой в конфиге или
  на лету из D-Bus/трея: быстрый zipformer-ru для тишины, GigaAM v3 для шума.
* **Внешнее управление по D-Bus** — сервис `org.voiceassistant.App`.

## Требования

| Компонент | Версия | Зачем |
|---|---|---|
| Linux, X11 | — | для Wayland/TTY потребуется `ydotool` (в планах) |
| g++ / clang | C++17 | сборка |
| CMake | 3.16+ | сборка |
| Qt | 5.13+ (Core, Widgets, Multimedia, DBus, Test) | каркас, звук, трей, D-Bus |
| sherpa-onnx | с C++ API (`cxx-api.h`) | VAD и ASR |
| xdotool, xclip | любые | вставка текста |
| Свободное место | ~300 МБ | модели (VAD 0.6 МБ + ASR 110–208 МБ) |

## Установка

### 1. Системные зависимости

```bash
sudo apt install build-essential cmake \
    qtbase5-dev qtmultimedia5-dev \
    xdotool xclip
```

### 2. sherpa-onnx

Рекомендуется собрать из исходников тем же компилятором, что и проект — тогда
ABI libstdc++ гарантированно совпадёт:

```bash
git clone https://github.com/k2-fsa/sherpa-onnx.git
cd sherpa-onnx && mkdir build-shared && cd build-shared
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
         -DSHERPA_ONNX_ENABLE_TTS=OFF -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
         -DSHERPA_ONNX_ENABLE_TESTS=OFF
make -j$(nproc) && sudo make install && sudo ldconfig
```

Если ставите **готовые бинарники** sherpa-onnx, проверьте ABI — иначе приложение
соберётся без ошибок, но упадёт в `strlen()` при создании VAD:

```bash
nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs
#   >0  -> библиотека собрана со старым ABI, собирайте проект с
#          cmake .. -DSHERPA_ONNX_OLD_CXX_ABI=ON
#    0  -> обычный новый ABI, ничего добавлять не нужно
```

Подробности — в [docs/sherpa-onnx-notes.md](docs/sherpa-onnx-notes.md).

### 3. Модели

```bash
mkdir -p ~/.voice_models && cd ~/.voice_models

# VAD (обязательно, 0.6 МБ)
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx

# ASR по умолчанию: русский zipformer (110 МБ)
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2
tar xf sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2 && rm sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2

# словарь для hotwords обычными словами (нужен только для zipformer-ru)
python3 -m pip install --user sentencepiece
python3 <путь к проекту>/tools/gen_bpe_vocab.py \
    ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.model \
    ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.vocab
```

Запасные профили (GigaAM v3 RNN-T для шумной обстановки, Whisper для английского)
и сравнение моделей по точности и скорости — в [docs/models.md](docs/models.md).

### 4. Конфиг

```bash
mkdir -p ~/.config/voice-assistant
cp config/* ~/.config/voice-assistant/
```

### 5. Сборка

```bash
git clone https://github.com/mamon-t/voice-assistant.git
cd voice-assistant
mkdir -p build && cd build
cmake ..              # + -DSHERPA_ONNX_OLD_CXX_ABI=ON, если проверка ABI дала >0
make -j$(nproc)
ctest --output-on-failure
```

## Быстрый старт

```bash
./src/voice-assistant              # значок в системном лотке
```

* Клик по значку — начать/остановить диктовку.
* Правый клик — выбор режима (диктовка / правка / проверка правописания),
  настройки, редактор подсказок, выход.
* Говорите обычный текст — он появляется в активном поле ввода.
* Скажите «режим редактирования», затем «удали слово», «новая строка» и т. п.

Управление из терминала:

```bash
BUS=org.voiceassistant.App
OBJ=/org/voiceassistant/App
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.getMode
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.setMode "dictation"
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.reloadHotwords
gdbus monitor --session --dest $BUS      # смотреть сигналы textRecognized
```

Проверка тракта без микрофона и X-сервера:

```bash
VOICE_ASSISTANT_DRYRUN=1 ./src/voice-assistant     # инжектор логирует вместо отправки
./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
```

## Настройка

Всё — в `~/.config/voice-assistant/settings.ini`. Профили ASR описаны секциями
`[asr_<имя>]`, активный выбирается строкой `active=` в секции `[asr]`.

```ini
[asr]
active=zipformer-ru
profiles=zipformer-ru,gigaam-v3,gigaam-v3-ctc,whisper-base
```

Пути к моделям можно задавать относительно `~/.voice_models`, от home (`~/...`)
или абсолютно. Полный справочник всех ключей — в
[docs/configuration.md](docs/configuration.md).

## Голосовые команды

| Фраза | Действие |
|---|---|
| «режим диктовки» / «режим ввода» | `Mode::Dictation` |
| «режим редактирования» / «режим правки» | `Mode::Edit` |
| «режим проверки» / «режим правописания» | `Mode::Spellcheck` |
| «выключить» / «стоп» | `Mode::Off` |
| «удали слово» / «сотри слово» | `Ctrl+BackSpace` |
| «удали строку» | `Shift+Home`, `BackSpace` |
| «новая строка» / «абзац» | `Enter` |
| «пробел» | пробел |
| «точка», «запятая», «двоеточие», «тире» … | соответствующий знак |

Командой считается только фраза, совпавшая со словарём **целиком**: «привет мир» —
это текст, «удали слово» — команда. Команды правки в режиме диктовки по умолчанию
выключены (`[commands] editing_in_dictation=false`), иначе продиктованное
«удали слово» съедало бы само себя. Свои команды добавляются файлом
`фраза = тип[:аргумент]` — см. [docs/configuration.md](docs/configuration.md).

## Инструменты

| Файл | Назначение |
|---|---|
| `tools/vad_asr_test.cpp` | прогон WAV через боевой тракт (конфиг → VAD → ASR → пунктуация), печатает сегменты, текст и RTF |
| `tools/gen_bpe_vocab.py` | делает из `bpe.model` словарь для hotwords обычными словами |

```bash
./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini \
    [--profile gigaam-v3] [--hotwords words.txt] [--threads 1] [--no-punct]
```

## Тесты

```bash
cd build && ctest --output-on-failure
```

* `CommandParserTest` — разбор команд;
* `VoiceUnitsTest` — словарь, нормализация, постобработка текста (голосовые знаки,
  заглавные, пробелы), инжектор в режиме dry-run.

## Архитектура

```
микрофон → QtAudioCapture → Agc → ApplicationController
                                      ↓
                                 VoicePipeline
                     SileroVad → сегмент → IRecognizer → TextPostProcessor
                                      ↓
                    CommandParser (команда)  /  ITextInjector (текст)
```

* `ApplicationController` тонкий: режимы, трей, D-Bus, маршрутизация результата.
* Выбор модели — за `ConfigManager` + `RecognizerFactory`; всё приложение знает
  только интерфейс `IRecognizer`.
* `SileroVad` отдаёт **целые речевые сегменты**: нестриминговая модель получает
  фразу целиком, а не нарезку по 32 мс.

Описание слоёв, порядка сигналов и владения объектами —
в [docs/architecture.md](docs/architecture.md).

## Производительность

Замер на русской фразе 7.16 с (2 потока, серверный CPU; на Celeron умножайте на 3–5):

| Профиль ASR | Размер | Декод | RTF | Русский |
|---|---|---|---|---|
| **zipformer-ru** (по умолчанию) | 110 МБ | **397 мс** | **0.06** | точно |
| GigaAM v3 RNN-T | 167 МБ | 1905 мс | 0.27 | точнее на шуме |
| GigaAM v3 CTC | 163 МБ | 1256 мс | 0.16 | точно, без hotwords |
| Whisper base | 208 МБ | 3663 мс | 0.48 | хуже, зато пунктуация и английский |

Полные таблицы, методика и вывод, почему SenseVoice не подходит для русского, —
в [docs/models.md](docs/models.md).

## Дорожная карта

* [ ] `Mode::Spellcheck`: `src/spellcheck/AspellChecker.cpp` пока пустой
* [ ] Push-to-talk: глобальный хоткей через `evdev` (сейчас режимы из трея и D-Bus)
* [ ] Wayland/TTY: реализация `ITextInjector` поверх `ydotool`
* [ ] Частичные результаты в реальном времени (streaming zipformer)
* [ ] Пунктуация моделью, если появится русская (API `cxx::OfflinePunctuation` уже есть)

## Документация

| Файл | О чём |
|---|---|
| [docs/architecture.md](docs/architecture.md) | слои, цепочка сигналов, владение объектами |
| [docs/configuration.md](docs/configuration.md) | справочник `settings.ini`, профили, пути, команды |
| [docs/models.md](docs/models.md) | модели, лицензии, замеры точности и скорости |
| [docs/hotwords-and-punctuation.md](docs/hotwords-and-punctuation.md) | подсказки пользователя и пунктуация |
| [docs/sherpa-onnx-notes.md](docs/sherpa-onnx-notes.md) | реальный API sherpa-onnx, ABI, AUTOMOC |
| [docs/integration-ApplicationController.md](docs/integration-ApplicationController.md) | как устроен контроллер |
| [docs/troubleshooting.md](docs/troubleshooting.md) | симптомы и лечение |
| [docs/development-log.md](docs/development-log.md) | рабочие заметки по итерациям |

## Лицензия

Код проекта — [MIT](LICENSE).

Сторонние компоненты распространяются по своим лицензиям и **не входят** в репозиторий
(скачиваются отдельно):

| Компонент | Лицензия |
|---|---|
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) | Apache-2.0 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT |
| [zipformer-ru (icefall)](https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-transducer/zipformer-transducer-models.html) | Apache-2.0 |
| [GigaAM v2/v3](https://github.com/salute-developers/GigaAM) | MIT |
| [Whisper](https://github.com/openai/whisper) | MIT |
| Qt 5 | LGPL-3.0 (динамическая линковка) |

## Благодарности

* [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) — инференс и готовые
  ONNX-модели, включая конвертацию GigaAM.
* [Salute Developers (GigaAM)](https://github.com/salute-developers/GigaAM) — лучшие
  открытые русские акустические модели.
* [Silero](https://github.com/snakers4/silero-vad) — VAD, который тянет слабое железо.
