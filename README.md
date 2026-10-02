# voice-assistant — VAD + ASR + конфиг профилей + интеграция (итерация 5)

Дата замеров: 2026-10-03. Все цифры — результат компиляции и запуска в песочнице.
Заголовки sherpa-onnx совпадают с твоими байт в байт: `cxx-api.h` 67 563 Б (66.0 KB),
`c-api.h` 169 199 Б (165.2 KB). Финальная сборка: **0 ошибок, 0 предупреждений**.

---

## 0. Интеграция в приложение (итерация 5)

Голосовой тракт собран воедино и **встроен в `ApplicationController`**. Проект
целиком компилируется (0 ошибок, 0 предупреждений), тесты проходят: `ctest` 2/2,
в сумме 23 проверки.

| Файл | Что сделано |
|---|---|
| `src/core/ApplicationController.{h,cpp}` | конфиг → AGC → `VoicePipeline` → команда или вставка текста; смена профиля ASR на лету; reloadHotwords |
| `src/commands/CommandDictionary.{h,cpp}` | 26 русских команд + загрузка своего файла `фраза = тип[:аргумент]` |
| `src/commands/CommandParser.{h,cpp}` | нормализация (регистр, пунктуация, дефис) и поиск команды по ВСЕЙ фразе |
| `src/output/XdotoolInjector.{h,cpp}` | настоящая вставка: буфер обмена (xclip/xsel/wl-copy) + Ctrl+V, либо `xdotool type --file -`; `VOICE_ASSISTANT_DRYRUN=1` для прогонов без X |
| `src/output/ITextInjector.h` | добавлен `backendName()` (не pure — чужие реализации не ломаются) |
| `src/config/HotwordsManager.cpp` | реализован `reload()` (был TODO) |
| `src/config/ConfigManager.{h,cpp}` | секции `[output]`, `[commands]`, `[audio]` |
| `src/dbus/DBusInterface.cpp` | `reloadHotwords()` + проброс `textRecognized`/`errorOccurred` (сигналы объявлялись, но никто их не слал) |
| `src/main.cpp` | регистрация D-Bus сервиса `org.voiceassistant.App` на `/org/voiceassistant/App` |
| `src/CMakeLists.txt` | цель `voice-assistant-core` (статическая библиотека) — её ждал `tests/`, теперь тесты собираются |
| `tests/test_voice_units.cpp` | 18 проверок на парсер, словарь, постпроцессор и инжектор |
| `config/settings.ini`, `config/commands_hotwords.txt` | новые секции и подсказки для команд |

Удалено: `src/asr/SenseVoiceRecognizer.{h,cpp}` (это и были 6 ошибок сборки —
старый код с несуществующим API), `config/ConfigManager.h` (забытый стаб с путями
SenseVoice вне `src/`).

### Маршрутизация результата

```
микрофон -> AGC -> VoicePipeline
                     |-> rawTextRecognized(raw)  -> CommandParser: ВСЯ фраза есть в словаре?
                     |                                да -> executeCommand(), текст НЕ вставляем
                     |-> textReady(text)          -> Mode::Dictation: инжектор.typeText(text)
                                                    Mode::Edit: только команды
                                                    Mode::Spellcheck: TODO (AspellChecker пустой)
```

Команда ищется по **сырому** тексту намеренно: `TextPostProcessor` превращает
«точка» и «новая строка» в знаки, и из готового текста команда уже не соберётся.

Команды правки («удали слово», «новая строка») в режиме диктовки по умолчанию
**не** выполняются — иначе продиктованное «удали слово» съедало бы само себя.
Включается `[commands] editing_in_dictation=true`. Команды смены режима работают всегда.

Фразы из словаря автоматически уходят в hotwords (`applyHotwords()`): без буста
ASR проглатывает служебные слова — тот же эффект, что с «точка»/«запятая».

### Запуск

```bash
sudo apt install xdotool xclip          # xclip — для вставки кириллицы без зависимости от раскладки
mkdir -p ~/.config/voice-assistant && cp config/* ~/.config/voice-assistant/
cd build && cmake .. && make -j4 && ctest --output-on-failure

# проверка ввода текста без X-сервера:
VOICE_ASSISTANT_DRYRUN=1 ./src/voice-assistant
```

### Что осталось сделать

* `Mode::Spellcheck`: `src/spellcheck/AspellChecker.cpp` **пустой файл** (0 байт),
  как и `src/core/Application.cpp` — их стоит либо наполнить, либо удалить
  (CMake на них ругается предупреждением AutoGen).
* Push-to-talk: глобальный хоткей через `evdev` (в текущей схеме режимы переключаются
  из трея и по D-Bus).
* Wayland/TTY: `ydotool` вместо `xdotool` (сейчас инжектор X11-специфичный;
  интерфейс `ITextInjector` позволяет добавить вторую реализацию без правок контроллера).

### Грабли, найденные при интеграции

1. **`QTextStream << "кириллица"` (узкий литерал) кодируется как Latin-1** — получается
   двойная UTF-8-кодировка. Везде нужен `QStringLiteral`/`QString`. На этом споткнулся
   мой же тест: файл команд записывался кракозябрами, а `added` при этом честно
   возвращал 3.
2. **Дефис в `normalize()`**: если его удалять, «режим-правки» превращается в «режимправки»
   и никогда не совпадёт со словарём. Теперь дефис/тире — разделитель. Нормализация
   одна на парсер и словарь (`CommandParser::normalize`), иначе они разъезжаются.
3. **`tests/` ссылался на несуществующую цель `voice-assistant-core`** — CMake не
   ругается на этапе конфигурации (принимает её за имя библиотеки), падает только
   компиляция на отсутствии include-путей.

---

## 0.1. Профили и движки (итерация 4)

| Файл | Зачем |
|---|---|
| `src/config/AsrProfile.h` | описание модели: движок, пути, потоки, метод декодирования |
| `src/config/ConfigManager.h/.cpp` | **профили ASR из `settings.ini`**, пути, настройки VAD и текста |
| `config/settings.ini` | шаблон с четырьмя готовыми профилями |
| `src/asr/RecognizerFactory.h/.cpp` | создаёт нужный `IRecognizer` по профилю |
| `src/asr/NemoCtcRecognizer.h/.cpp` | движок NeMo/GigaAM **CTC** (одна модель) |
| `src/core/VoicePipeline.h/.cpp` | цепочка VAD → ASR → постобработка → `textReady(QString)` |
| `src/text/TextPostProcessor.h/.cpp` | пунктуация: голосовые знаки + границы предложений |
| `docs/integration-ApplicationController.md` | готовый патч контроллера |
| `tools/vad_asr_test.cpp` | переведён на боевой путь `--config settings.ini` |

Логика слоёв: `ApplicationController` остаётся тонким (режимы, трей, инжектор),
вся речь живёт в `VoicePipeline`, конкретную модель выбирает конфиг. Переключение
модели — одна строка в `settings.ini` или `switchAsrProfile()` в рантайме.

---

## 1. Куда грузить модели в боевой обстановке

`ConfigManager::resolvePath()` понимает три формы записи, так что конфиг переносим:

```ini
encoder=sherpa-onnx-small-zipformer-ru-2024-09-18/encoder.int8.onnx   ; от ~/.voice_models
encoder=~/models/zipformer-ru/encoder.int8.onnx                       ; от home
encoder=/opt/voice-models/zipformer-ru/encoder.int8.onnx              ; абсолютный
```

Профили — секции `[asr_<имя>]`, активный — `[asr] active=...`, список — `[asr] profiles=...`.
Полный шаблон: `config/settings.ini` (там же комментарии, зачем каждый профиль).

```bash
# проверить, что конфиг и модели видны, без UI и микрофона:
./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
./tools/vad-asr-test запись.wav --config ... --profile gigaam-v3     # другой профиль
./tools/vad-asr-test запись.wav --config ... --hotwords user.txt --threads 1 --no-punct
```

Реальный вывод (песочница, 11.84 с русской речи с диктантными словами):

```
CFG : /tmp/test-settings.ini
      профили: zipformer-ru, gigaam-v3, gigaam-v3-ctc, whisper-base
      активен: zipformer-ru
Init: 5104 ms | ASR: zipformer-ru (transducer) | потоков: 2 | decoding: modified_beam_search
Hotwords: 6 шт. из /tmp/hw2.txt (score=2.0)
  #1  raw : привет это последняя итерация голосового помощника точка набор текста работает запятая ...
  #1  text: Привет это последняя итерация голосового помощника. Набор текста работает, ...речи?
Segments: 1 | audio 11.84 s | wall 713 ms | ASR 712 ms | RTF 0.06
```

---

## 2. Пунктуация: как решено

Русской модели пунктуации в sherpa-onnx нет (только `punct-ct-transformer-zh-en`),
а GigaAM с пунктуацией (`v3_e2e_*`) в ONNX не выложен. Поэтому:

1. **Голосовые знаки**: «точка» → `.`, «запятая» → `,`, «вопросительный знак» → `?`,
   «восклицательный знак» → `!`, «двоеточие», «точка с запятой», «тире», «дефис»,
   «абзац»/«новая строка» → перевод строки, «открывающая/закрывающая скобка».
   Правила — в `TextPostProcessor::defaultRules()`, матчинг самый длинный фразой вперёд.
2. **Граница сегмента VAD = граница предложения**: заглавная в начале, точка в конце,
   заглавная после `. ! ?`.
3. **Косметика**: пробел перед знаком убирается, повторы пробелов схлопываются.

### Важный эффект, найденный замером

ASR **проглатывает** диктантные слова: модель обучена на живой речи и не ждёт команд.
Без буста из «…помощника точка набор текста…» в тексте не осталось ни одной «точки».
Стоило добавить «точка», «запятая», «вопросительный знак» в hotwords — как модель
начала их выдавать, и пунктуация собралась:

| hotwords | raw от ASR | после постобработки |
|---|---|---|
| без диктантных слов | «…помощника набор текста работает запятая …речи вопросительный» | «…помощника набор текста работает, …речи?» |
| **с диктантными словами** | «…помощника **точка** набор текста работает **запятая** …речи **вопросительный знак**» | «…помощника**.** Набор текста работает**,** …речи**?**» |

Поэтому `VoicePipeline` примешивает диктантные знаки к hotwords **сам**
(`applyHotwords()`), когда включена `voice_punctuation` и движок — transducer
в `modified_beam_search`. Слово «пробел» в авто-буст не входит: бустить его рискованнее,
чем полезно.

---

## 3. Замеры: четыре профиля из одного конфига

Одна и та же русская запись 11.84 с, прогон через `VoicePipeline` (`--config`),
2 потока, серверный CPU (на Селероне умножать на 3–5):

| Профиль | Движок | Размер | Текст | ASR | RTF |
|---|---|---|---|---|---|
| **zipformer-ru** | transducer | 110 МБ | «Привет это последняя итерация голосового помощника. Набор текста работает, **они просто** распознавание речи?» | **713 мс** | **0.06** |
| **gigaam-v3** | transducer | 167 МБ | «Привет**.** Это последняя итерация голосового помощника. Набор текста работает, **а не просто** распознавание речи?» | 2210 мс | 0.19 |
| gigaam-v3-ctc | nemo-ctc | 163 МБ | точный, но без hotwords | 1256 мс | 0.16 |
| whisper-base | whisper | 208 МБ | «Привет! Это последняя итерация… **На борте к стаработают в любом поле в вода.**» | 3656 мс | 0.48 |

Что из этого следует:

* **zipformer-ru** — в 3 раза быстрее, но на этой записи ошибся («а не» → «они») и
  требует буста диктантных слов.
* **GigaAM v3** — текст идеальный, диктантные слова слышит **без** всякого буста
  (сам поставил «Привет.»), но в 3 раза медленнее: на Селероне RTF будет около 0.6–1.0,
  то есть фраза в 5 с декодируется 3–5 с. Для диктовки по сегментам терпимо,
  для комфортной работы впритык.
* Практичный вариант: **по умолчанию zipformer-ru**, а `gigaam-v3` — переключаемый
  профиль «точно» (кнопка в трее / горячая клавиша). Переключение — `switchAsrProfile()`,
  стоит 1.2–5.1 с на переинициализацию, ОЗУ предыдущей модели освобождается.
* `gigaam-v3-ctc` оставлен для полноты матрицы: чуть быстрее RNN-T, но hotwords не поддерживает.

---

## 4. Что делать с Whisper

**Оставляем запасным профилем, по умолчанию не активен.** Аргументы:

* единственный из проверенных, кто сам ставит пунктуацию и заглавные и нормализует числа
  («October 3, 2026», «about 25 source files») — полезно, если наша постобработка не нужна;
* единственный мультиязычный: английский у него отличный
  («Hello, Mikael. This is the final iteration of the Voice Assistant.»),
  а диктовка англоязычных терминов/коммитов у разработчика случается;
* в профильной схеме он **ничего не стоит**, пока не выбран: модель не грузится,
  память не ест;
* русский у него плохой и он самый медленный (RTF 0.48) — основным быть не может.

Цена хранения — два файла (`WhisperRecognizer.h/.cpp`, ~250 строк) и одна секция в
`settings.ini`. Если захочешь нулевую мёртвую массу: удалить эти два файла, строку
`WhisperRecognizer.cpp` из `tools/CMakeLists.txt`, ветку `case Engine::Whisper` в
`RecognizerFactory.cpp`, перечисление `Whisper` в `AsrProfile` и секцию `[asr_whisper-base]`.

---

## 5. Две грабли, из-за которых сборка падала раньше

### 5.1 `file(GLOB "*.cpp")` ломает линковку

`IVad` / `IRecognizer` — header-only классы с `Q_OBJECT`. Без заголовков в списке
источников AUTOMOC не создаёт `moc_IVad.cpp`:

```
undefined reference to `IVad::speechStarted()'
undefined reference to `vtable for IVad'
```

Исправлено: `*.h` добавлены в источники в `src/CMakeLists.txt` и `tools/CMakeLists.txt`.

### 5.2 ABI libstdc++ — segfault без единой ошибки компиляции

Официальные **предсобранные** sherpa-onnx собраны со старым ABI (в манглинге
`...12CreateStreamERKSs`, `Ss` = `_GLIBCXX_USE_CXX11_ABI=0`). Тогда
`sizeof(cxx::VadModelConfig)` = 160 в нашем коде и 88 в библиотеке, она читает
`ten_vad.model` из наших float'ов и падает в `strlen()`:

```
#0  vpcmpeqb (%rdi),%ymm16,%k0        # libc; rdi = 0x3f0000003f000000 = два float 0.5f
#1  SherpaOnnxCreateVoiceActivityDetector ()
#2  sherpa_onnx::cxx::VoiceActivityDetector::Create ()
#3  SileroVad::initialize ()
```

Проверка:

```bash
nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs
```

* `> 0` → old-ABI → собирать с `-DSHERPA_ONNX_OLD_CXX_ABI=ON` (опция есть в CMakeLists,
  с Qt5 сочетается — все прогоны выше сделаны именно так);
* `0` → new-ABI (обычно так, если sherpa-onnx собиралась из исходников твоим же g++) → `OFF`.

---

## 6. Файлы

```
CMakeLists.txt                        поиск sherpa-onnx, опция ABI, rpath, tools/tests
config/settings.ini                   4 профиля ASR + [vad] [text] [output] [commands] [audio]
config/commands_hotwords.txt          диктантные команды как подсказки для ASR
src/CMakeLists.txt                    цель voice-assistant-core + *.h в источниках (AUTOMOC/vtable)
tests/test_voice_units.cpp            18 проверок: парсер, словарь, постпроцессор, инжектор

src/config/AsrProfile.h               профиль модели + проверка путей
src/config/ConfigManager.h/.cpp       settings.ini: профили, пути, VAD, текст, вывод, команды
src/config/HotwordsManager.h/.cpp     reload() реализован: командные + пользовательские подсказки
src/commands/CommandDictionary.h/.cpp 26 команд + свой файл «фраза = тип[:аргумент]»
src/commands/CommandParser.h/.cpp     нормализация и поиск команды по всей фразе
src/core/ApplicationController.h/.cpp режимы, AGC, пайплайн, команды, инжектор, D-Bus-сигналы
src/output/ITextInjector.h            интерфейс + backendName()
src/output/XdotoolInjector.h/.cpp     буфер обмена + Ctrl+V, либо xdotool type; DRY RUN
src/dbus/DBusInterface.cpp            reloadHotwords + проброс textRecognized/errorOccurred
src/main.cpp                          регистрация D-Bus сервиса
src/asr/IRecognizer.h                 интерфейс (эталон)
src/asr/RecognizerFactory.h/.cpp      профиль -> конкретный распознаватель
src/asr/TransducerRecognizer.h/.cpp   zipformer-ru, GigaAM RNN-T + hotwords
src/asr/NemoCtcRecognizer.h/.cpp      GigaAM CTC / NeMo CTC
src/asr/WhisperRecognizer.h/.cpp      запасной мультиязычный
src/vad/IVad.h                        интерфейс (эталон)
src/vad/SileroVad.h/.cpp              Silero VAD через cxx::VoiceActivityDetector
src/core/VoicePipeline.h/.cpp         VAD -> ASR -> текст, авто-hotwords для пунктуации
src/text/TextPostProcessor.h/.cpp     голосовые знаки, заглавные, точка в конце

tools/CMakeLists.txt                  отдельный таргет, src/ не трогает
tools/vad_asr_test.cpp                CLI-стенд на боевом пути (--config)
tools/gen_bpe_vocab.py                словарь ssentencepiece для hotwords
docs/integration-ApplicationController.md   патч контроллера
```

Удалено: `src/asr/SenseVoiceRecognizer.*` (модель не знает русского).
`src/onnx/OnnxEnvironment.*` — мёртвый код, можно удалить.

Интерфейсы `IVad` / `IRecognizer` не менялись. Требования к ним: конструктор
`explicit IXxx(QObject* parent = nullptr) : QObject(parent) {}`.

---

## 7. Сборка

```bash
# модели
mkdir -p ~/.voice_models && cd ~/.voice_models
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2
tar xf sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2
# запасной профиль (MIT):
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16.tar.bz2
tar xf sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16.tar.bz2

# словарь для hotwords словами (только для zipformer-ru)
pip install sentencepiece
python3 <проект>/tools/gen_bpe_vocab.py \
    ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.model \
    ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.vocab

# конфиг
mkdir -p ~/.config/voice-assistant
cp <проект>/config/settings.ini ~/.config/voice-assistant/

# сборка
nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs   # >0 => OLD_CXX_ABI=ON
cd build && cmake .. && make -j4

# проверка без микрофона
./tools/vad-asr-test ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/test_wavs/0.wav \
    --config ~/.config/voice-assistant/settings.ini
```

Дальше — `docs/integration-ApplicationController.md`.
