# Рабочие заметки

Хронология решений и ошибок. Держим отдельно от README: здесь то, как проект
устроен **изнутри** и на какие грабли мы наступили, а не как им пользоваться.

---

## Итерация 1. Выбор архитектуры

Задача: диктовка и правка текста голосом в Linux, в любом поле ввода, офлайн,
на слабом железе (Celeron, 16 ГБ), с обучением на подсказках пользователя
и с голосовыми командами режимов.

Решения:

* **ASR/VAD — sherpa-onnx, а не Vosk.** Vosk проще, но у sherpa-onnx есть
  и Silero VAD, и несколько семейств offline-моделей, и C++ API, и готовые
  ONNX-модели под русский. Один источник зависимостей вместо двух.
* **Ввод текста — не `xdotool` в лоб.** Для X11 он годится, но для Wayland
  и голой TTY нужен `ydotool` (`/dev/uinput`). Поэтому ввод спрятан за
  интерфейс `ITextInjector` — смена бэкенда не трогает контроллер.
* **Команды — детерминированный словарь, а не классификатор интентов.**
  На Celeron нейросетевой классификатор лишний, а словарь покрывает задачу
  и расширяется файлом.
* **Каркас — Qt5** (`QAudioInput`, трей, `QSettings`, D-Bus через
  `QDBusAbstractAdaptor`), C++17.

## Итерация 2. Скелет

Интерфейсы `IAudioCapture`, `IVad`, `IRecognizer`, `ITextInjector`,
`ISpellChecker`, классы `ApplicationController`, `Agc`, `RingBuffer`,
`TrayIcon`, `ConfigManager`, `HotwordsManager`, заглушки реализаций, QTest.

## Итерация 3. Интеграция sherpa-onnx: десять ошибок API

Первая реализация VAD и ASR писалась по памяти и не собралась. Реальный API
сверили с установленными заголовками (`cxx-api.h` 66.0 KB, `c-api.h` 165.2 KB)
и исходниками sherpa-onnx.

| Было | Стало |
|---|---|
| `VoiceActivityDetectorConfig` | `VadModelConfig` |
| `config.model_config.silero_vad.*` | `config.silero_vad.*` |
| `make_unique<VoiceActivityDetector>(config)` | `VoiceActivityDetector::Create(config, seconds)` |
| `vad->Detected()` | `vad->IsDetected()` |
| `make_unique<OfflineRecognizer>(config)` | `OfflineRecognizer::Create(config)` |
| `rec->DecodeStream(&s)` | `rec->Decode(&s)` |
| `s.GetResult()` | `rec.GetResult(&s)` |
| `try/catch` вокруг `Create` | проверка `Get() == nullptr` (исключений C API не бросает) |
| свой `Ort::Env`/`Ort::Session` для Silero | VAD из sherpa-onnx, ONNX Runtime отдельно не нужен |
| `signals: void speechStarted() override;` | сигналы наследуются из `IVad`, `override` недопустим |

Отдельно: один `OfflineStream` на каждый чанк аудио и склейка текста давали
рваный результат — нестриминговую модель надо кормить целой фразой.

## Итерация 4. Модели: SenseVoice не знает русского

Изначально ставили SenseVoice-small, исходя из того, что «русский работает
хорошо». Проверка это опровергла. Замер на одной и той же русской фразе,
официальным бинарником `sherpa-onnx-offline` (то есть обёртка ни при чём):

```
SenseVoice     : RIVIE PALII IT RDS A GLSS OF OONICAL BONE POEODA   (lang=<|yue|>)
SenseVoice, zh : 开放时间早上九点至下午五点                            (идеально)
Whisper tiny   : На бортех стар работает в любом поле вода
Whisper base   : На борте к стаработают в любом поле в вода
zipformer-ru   : набор текста работает в любом поле ввода            ✓
GigaAM v3      : набор текста работает в любом поле ввода            ✓
```

SenseVoice удалена. Основным движком стал `small-zipformer-ru` (110 МБ,
RTF 0.06), запасным — GigaAM v3 (MIT, точнее на шуме, в 3–5 раз медленнее).
Подробные таблицы — в [models.md](models.md).

Там же выяснилось:

* GigaAM доступен в sherpa-onnx в готовом ONNX (4 модели), Python в рантайме
  не нужен;
* пунктуацию умеют только `v3_e2e_*`, которых в ONNX нет;
* hotwords работают лишь у transducer-моделей и лишь в `modified_beam_search`;
* `modified_beam_search` точнее `greedy_search` (greedy терял начало фразы).

## Итерация 5. Профили, пайплайн, интеграция

* `AsrProfile` + `RecognizerFactory` — модель выбирается конфигом, приложение
  знает только `IRecognizer`.
* `VoicePipeline` — вся речевая цепочка в одном классе, контроллер остаётся тонким.
* `TextPostProcessor` — голосовые знаки, заглавные, точка в конце.
* `TransducerRecognizer` (бывший `ZipformerRecognizer`) — переименован, когда
  оказалось, что тем же конфигом грузится и GigaAM RNN-T.
* `NemoCtcRecognizer` — для GigaAM CTC.
* Реализованы заглушки: `CommandParser`, `CommandDictionary` (26 команд),
  `XdotoolInjector`, `HotwordsManager::reload()`, `DBusInterface::reloadHotwords()`.
* `src/CMakeLists.txt` собирает `voice-assistant-core` — цель, которую `tests/`
  ждал с самого начала.

Ключевой эксперимент итерации: ASR **проглатывает** диктантные слова, если их
не бустить. Добавление «точка», «запятая», «вопросительный знак» в hotwords
вернуло их в распознавание, после чего голосовая пунктуация заработала.
`VoicePipeline` примешивает эти слова к подсказкам пользователя автоматически.

---

## Реестр граблей

То, что стоило времени. Каждая оформлена там, где её найдут по симптому.

| # | Грабля | Симптом | Где описана |
|---|---|---|---|
| 1 | Предсобранные sherpa-onnx собраны со старым ABI libstdc++ | segfault в `strlen` из `Create()`, компиляция чистая | [sherpa-onnx-notes.md](sherpa-onnx-notes.md) |
| 2 | Header-only классы с `Q_OBJECT` не видны AUTOMOC при `GLOB "*.cpp"` | `undefined reference to vtable for IVad` | там же |
| 3 | CMake принимает несуществующую цель за имя библиотеки | тесты не находят заголовки | там же |
| 4 | API sherpa-onnx написан по памяти | 10 ошибок компиляции | там же |
| 5 | SenseVoice не поддерживает русский | каша вместо текста | [models.md](models.md) |
| 6 | `QTextStream << "кириллица"` (узкий литерал) кодируется как Latin-1 | двойная UTF-8-кодировка файла | [troubleshooting.md](troubleshooting.md) |
| 7 | `normalize()` удаляла дефис вместо замены на пробел | «режим-правки» никогда не совпадал со словарём | — |
| 8 | `xdotool type` зависит от раскладки | «ghbdtn» вместо «привет» | [troubleshooting.md](troubleshooting.md) |
| 9 | `QSettings` возвращает `QStringList` для значений с запятыми | `profiles` читался как пустая строка | [configuration.md](configuration.md) |
| 10 | `= Options()` в аргументе по умолчанию того же класса | GCC: «default member initializer required before the end of its enclosing class» | — |
| 11 | Сегменты VAD не выгребать через `Pop()` | неограниченный рост очереди внутри детектора | [architecture.md](architecture.md) |
| 12 | QObject-родитель + `unique_ptr` на одном объекте | риск double free при рефакторинге | [architecture.md](architecture.md) |

Отдельная история: первый тест словаря команд «проходил» по счётчику
(`added == 3`), но фразы не находились — из-за грабли №6 файл команд
записывался кракозябрами. Счётчик при этом не врал: структурно строки
разбирались правильно. Вывод — в тестах проверять не только количество,
но и содержимое.

## Что дальше

* `AspellChecker` — сейчас `.cpp` пустой (0 байт), а заголовок объявляет
  конструктор, деструктор и три виртуальных метода. Пока класс никто не
  использует, сборка проходит; при подключении `Mode::Spellcheck` будет
  `undefined reference to vtable for AspellChecker`.
* `src/core/Application.cpp` тоже пустой — наполнить или удалить.
* Push-to-talk: глобальный хоткей через `evdev`.
* Wayland/TTY: реализация `ITextInjector` поверх `ydotool`.
* Стриминговое распознавание с частичными результатами — потребуется
  worker-поток и `Qt::QueuedConnection`.
* Таблица голосовых знаков сейчас в коде (`TextPostProcessor::defaultRules()`) —
  стоит вынести в конфиг.
* Русская модель пунктуации: API `cxx::OfflinePunctuation` в sherpa-onnx есть,
  модели нет. Как появится — подключится отдельным классом.
