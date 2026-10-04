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
  Прежнее содержимое буфера запоминается и возвращается обратно
  (`[output] preserve_clipboard`).
* **Полностью офлайн** — VAD (Silero) и ASR (zipformer-ru / GigaAM / Whisper)
  выполняются локально через [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx).
* **Голосовые команды** — смена режима, удаление слова/строки, новая строка,
  знаки препинания. Словарь расширяется своим файлом.
* **Пунктуация голосом** — «точка», «запятая», «вопросительный знак», «абзац»
  превращаются в знаки; заглавные буквы и точка в конце расставляются автоматически.
* **Подсказки пользователя (hotwords)** — редкие слова, имена и термины из файла
  повышают точность распознавания: для transducer-моделей работает настоящий
  контекстный бустинг, а не замена по тексту.
* **Проверка правописания** — режим `Spellcheck` проверяет продиктованное через
  hunspell с русским словарём и показывает варианты исправления уведомлением.
* **Заметки в файл** — вторая цель вывода: распознанный текст дописывается в
  `~/.local/share/voice-assistant/notes/YYYY-MM-DD.md` вместо активного окна.
  Ни фокус, ни клавиатура, ни буфер обмена не участвуют, поэтому пометки к файлу,
  с которым работаешь, можно диктовать, продолжая печатать руками. Переключение —
  голосом («заметка» / «в редактор»), из трея или по D-Bus.
* **Диктовка не блокирует клавиатуру** — `evdev` только слушает события
  (`[hotkey] grab=false` по умолчанию), а `EVIOCGRAB` дополнительно защищён:
  устройство, похожее на полноценную клавиатуру, не перехватывается, даже если
  в конфиге стоит `grab=true`, пока оно явно не названо в `grab_devices`.
* **Печать во время диктовки** — `[output] pin_window` привязывает вставку к окну,
  активному в начале записи, а `[audio] typing_guard_ms` глушит микрофон, пока
  идёт набор текста, чтобы стук клавиш не превращался в слова. Окна самого
  помощника (меню трея) привязкой не становятся, а каждая вставка пишется в лог
  с адресом и способом — «текст ушёл неизвестно куда» больше не диагностируется
  на глаз.
* **Глобальный хоткей микрофона** — `evdev`, работает в X11, Wayland и голой TTY;
  два режима: push-to-talk (держим клавишу) и toggle (нажал/отжал).
* **Проверка микрофона** — запись тракта в WAV из трея, по D-Bus или из консоли
  (`--record 15 ~/mic.wav`): тот же файл потом прогоняется всеми профилями ASR,
  что отделяет проблемы микрофона и AGC от проблем модели.
* **Переключаемые профили ASR** — модель меняется одной строкой в конфиге или
  на лету из D-Bus/трея: быстрый zipformer-ru для тишины, GigaAM v3 для шума.
* **Мастер загрузки моделей** — при первом запуске приложение само предложит
  скачать модели (список с размером и описанием, sha256, докачка при обрыве,
  атомарная распаковка); из консоли — `--download-model`.
* **Готовые пакеты .deb/.rpm** — sherpa-onnx/onnxruntime/hunspell едут внутри
  пакета, чистое удаление (см. «Установка»).
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
| доступ к `/dev/input` | группа `input` | глобальный хоткей через evdev |
| hunspell + словарь | libhunspell-dev, hunspell-ru | проверка правописания (необязательно) |
| Свободное место | ~300 МБ | модели (VAD 0.6 МБ + ASR 110–208 МБ) |

## Установка

### 0. Готовые пакеты (рекомендуется)

Скачайте `.deb` (Debian/Ubuntu/Mint) или `.rpm` (Fedora/openSUSE) со страницы
[Releases](https://github.com/mamon-t/voice-assistant/releases):

```bash
sudo apt install ./voice-assistant_0.9.0_amd64.deb    # Debian/Ubuntu/Mint
sudo dnf install ./voice-assistant-0.9.0-1.x86_64.rpm # Fedora
```

Пакет самодостаточен: sherpa-onnx, onnxruntime и hunspell едут внутри
(`/usr/lib/voice-assistant/`, RPATH `$ORIGIN`), системные библиотеки не
подменяются. При первом запуске помощник предложит **мастер загрузки моделей**
(~110 МБ, проверка sha256, докачка при обрыве); из консоли то же самое —
`voice-assistant --download-model zipformer-ru`. Для глобального хоткея
потребуется `sudo usermod -aG input $USER` и перелогин (postinst об этом
напомнит). Удаление: `sudo apt remove voice-assistant` — системные файлы
сносятся начисто; пользовательские данные (настройки, заметки, модели)
остаются в домашнем каталоге — как их убрать, см. конец раздела.

### 1. Системные зависимости

```bash
sudo apt install build-essential cmake \
    qtbase5-dev qtmultimedia5-dev \
    xdotool xclip

# необязательно, для режима проверки правописания:
sudo apt install libhunspell-dev hunspell-ru
```

Без hunspell проект тоже собирается — режим `Spellcheck` просто сообщит,
что поддержка не включена (зависимость детектируется через pkg-config,
см. вывод `cmake`).

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

Основной способ — **мастер загрузки в самом приложении**: при первом запуске
без моделей он откроется сам (или лоток → «Скачать модели...»). Проверка
sha256, докачка при обрыве, профиль автоматически становится активным.
Из консоли:

```bash
voice-assistant --download-model list            # что есть и что установлено
voice-assistant --download-model zipformer-ru    # VAD добавится автоматически
```

Вручную (альтернатива, например без GUI):

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

### Полное удаление (пакет и данные)

`sudo apt remove voice-assistant` (или `dnf remove`) сносит все системные
файлы начисто. Пользовательские данные пакет не трогает ни при remove, ни при
purge — так устроены дистрибутивы. Убираются одной командой:

```bash
rm -rf ~/.config/voice-assistant ~/.local/share/voice-assistant ~/.voice_models
```

## Быстрый старт

```bash
./src/voice-assistant --check      # диагностика: модели, инжектор, словарь, хоткей, /dev/input
./src/voice-assistant              # значок в системном лотке
./src/voice-assistant --record 15 ~/mic.wav   # записать 15 с с микрофона (сырой тракт, без AGC)
./src/voice-assistant --note "проверить AGC"  # дописать строку в файл заметок и выйти
./src/voice-assistant --pin-info              # куда пойдёт текст: окно, WM_CLASS, способ вставки
./src/voice-assistant --type "проверка раз" --pin-active --delay 3000
                                             # вставить текст, не запуская распознавание
```

Для глобального хоткея нужен доступ к устройствам ввода:

```bash
sudo usermod -aG input $USER      # затем перелогин
./src/voice-assistant --check     # строка «/dev/input: N устройств, доступно для чтения: M»
```

Без прав на `/dev/input` приложение работает как ни в чём не бывало — режимы
переключаются из трея и по D-Bus, хоткей просто не запускается.

`--check` не требует ни микрофона, ни X-сервера и печатает по каждому профилю
ASR все пути с пометкой `OK (размер)` или `НЕТ ФАЙЛА`, а в конце — число проблем.
Код возврата 0/1, поэтому его можно гонять в скриптах. Если приложение запустилось,
но речь не распознаётся, начинать надо именно с него.

* Клик по значку — начать/остановить диктовку.
* `F8` (по умолчанию) — push-to-talk: держим, пока говорим; отпустили — фраза
  ушла в распознавание. Клавиша и режим настраиваются в `[hotkey]`.
* Правый клик — выбор режима (диктовка / правка / проверка правописания),
  «Писать в файл заметок», «Открыть заметки», настройки, редактор подсказок, выход.
* Говорите обычный текст — он появляется в активном поле ввода.
* Скажите «заметка» — и текст пойдёт в файл заметок, а не в окно: можно
  продолжать печатать руками. Обратно — «в редактор».
* Скажите «режим редактирования», затем «удали слово», «новая строка» и т. п.
* В режиме «Проверка орфографии» текст вставляется как при диктовке, а найденные
  ошибки показываются уведомлением с вариантами: `прверка → проверка, поверка`.

`--check` помимо моделей и инжектора проверяет словарь и тут же прогоняет
контрольное слово:

```
Правописание
  движок    : hunspell
  словарь   : /usr/share/hunspell/ru_RU.dic
  контроль  : "прверка" -> ошибка, варианты: проверка, поверка, привертка
```

Управление из терминала:

```bash
BUS=org.voiceassistant.App
OBJ=/org/voiceassistant/App
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.getMode
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.setMode "dictation"
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.reloadHotwords
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.setOutputTarget "notes"
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.notesFile
gdbus monitor --session --dest $BUS      # смотреть сигналы textRecognized, noteWritten
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
| «заметка» / «в заметки» | цель вывода — файл заметок |
| «в редактор» / «в окно» | цель вывода — активное окно |
| «удали слово» / «сотри слово» | `Ctrl+BackSpace` |
| «удали строку» | `Shift+Home`, `BackSpace` |
| «новая строка» / «абзац» | `Enter` |
| «пробел» | пробел |
| «точка», «запятая», «двоеточие», «тире» … | соответствующий знак |

Командой считается только фраза, совпавшая со словарём **целиком**: «привет мир» —
это текст, «удали слово» — команда, а «сделать заметку на полях» — снова текст.
Команды правки в режиме диктовки по умолчанию выключены
(`[commands] editing_in_dictation=false`), иначе продиктованное «удали слово»
съедало бы само себя. Команды смены режима и цели вывода работают всегда:
это маршрутизация, а не правка текста. Если слово «заметка» мешает в обычной
речи, голосовое переключение выключается (`[notes] voice_commands=false`),
а цель остаётся доступной из трея и по D-Bus. Свои команды добавляются файлом
`фраза = тип[:аргумент]` — см. [docs/configuration.md](docs/configuration.md).

## Инструменты

| Файл | Назначение |
|---|---|
| `tools/vad_asr_test.cpp` | прогон WAV через боевой тракт (конфиг → VAD → ASR → пунктуация), печатает сегменты, текст и RTF |
| `tools/gen_bpe_vocab.py` | делает из `bpe.model` словарь для hotwords обычными словами |
| `./src/voice-assistant --check` | диагностика конфигурации, моделей, инжектора, словаря и хоткея |
| `./src/voice-assistant --record N file.wav` | запись с микрофона в WAV без GUI (сырой тракт, без AGC) |
| `./src/voice-assistant --note "текст"` | дописать заметку в файл из консоли (без микрофона и X) |
| `./src/voice-assistant --pin-info` | диагностика вывода: активное окно, WM_CLASS, PID, `pin_mode`, что запомнил бы `pin_window` |
| `voice-assistant --download-model list\|<id>\|all [--force]` | каталог моделей и загрузка: sha256, докачка, атомарная распаковка |
| `./src/voice-assistant --type "текст" [--pin-active] [--window WID] [--delay мс]` | проверить вставку отдельно от распознавания |
| `./tools/vad-asr-test … --notes` | то же, но заметками становятся сегменты из WAV: прогон записи в файл заметок |

```bash
./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini \
    [--profile gigaam-v3] [--hotwords words.txt] [--threads 1] [--no-punct] \
    [--notes] [--notes-file ~/work/notes.md]
```

## Тесты

```bash
cd build && ctest --output-on-failure
```

* `CommandParserTest` — разбор команд;
* `VoiceUnitsTest` — 66 проверок: словарь и нормализация команд, постобработка текста
  (голосовые знаки, заглавные, пробелы), разбор `settings.ini` с профилями и путями,
  инжектор в режиме dry-run, правописание по настоящему словарю
  (пропускается через `QSKIP`, если hunspell или словаря нет),
  запись WAV (заголовок, round-trip данных, валидность для внешних инструментов),
  логика evdev-хоткея на синтетических `input_event` — железо для тестов не нужно.
  Отдельно покрыты: файл заметок (`FileInjector`: имя файла дня, заголовок,
  метка времени, кириллица, «удали слово», явный файл без markdown),
  команды переключения цели и их отключение, `TypingGuard` на синтетическом
  времени, сигнал `keyActivity` и политика `EVIOCGRAB` — в том числе то, что
  основная клавиатура не перехватывается.
  Отдельно покрыт выбор окна для вставки (`output/WindowTarget.h`): куда пойдёт
  текст при привязке к окну — активация + настоящие события или `--window`
  с синтетикой, откат на активное окно, если привязанное закрыли или это окно
  самого помощника, возврат фокуса и разбор `pin_mode`. Это чистая функция без
  X11, поэтому проверяется без дисплея. Файл своих команд покрыт через
  `CommandDictionary::parseLine` (валидные строки, комментарии, битые типы)
  и round-trip `commandToSpec` по всем встроенным командам — той же функцией
  валидирует строки вкладка «Команды» перед сохранением. Каталог моделей
  (уникальность id, URL релизов, полные sha256, соответствие профилям) и
  `sha256File` загрузчика проверены на эталонном значении из FIPS 180-2.

Помимо модульных тестов есть e2e-стенд привязки вывода к окну —
`tools/e2e/run-e2e.sh` (13 сценариев): поднимает Xvfb + openbox, открывает
тестовые Qt-окна с перехватом сырых xcb-событий (видно, настоящими событиями
пришёл ввод или синтетикой `XSendEvent`) и прогоняет `--type` во всех
комбинациях `pin_mode`/`method`. В ctest не регистрируется — нужен X.
Подробности: `tools/e2e/README.md`.

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

* [x] `Mode::Spellcheck`: hunspell с русским словарём (`AspellChecker`-заглушка удалена)
* [x] Диалог настроек: пять вкладок (модель со статусом готовности, хоткей с захватом
      клавиши, вывод, подсказки, команды); встроенные команды видны как «фраза → действие»,
      свои — правятся с валидацией; всё применяется без перезапуска
* [ ] Автозамена найденной ошибки на первый вариант по голосовой команде
* [x] Push-to-talk: глобальный хоткей через `evdev` (X11, Wayland, TTY)
* [x] Заметки в файл как вторая цель вывода — диктовка не мешает печатать
* [ ] Панель-черновик: продиктованное копится в окне помощника и вставляется по команде
* [ ] Wayland/TTY: реализация `ITextInjector` поверх `ydotool` (хоткей там уже работает — evdev)
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
