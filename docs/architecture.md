# Архитектура

## Слои

```
┌─────────────────────────────────────────────────────────────────────┐
│ UI / внешнее управление                                              │
│   ui/TrayIcon        dbus/DBusInterface        main.cpp              │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ setMode(), startRecording(), …
┌───────────────────────────────▼─────────────────────────────────────┐
│ Ядро                                                                 │
│   core/ApplicationController — режимы, маршрутизация результата      │
└──────┬────────────────────────────┬──────────────────────┬──────────┘
       │                            │                      │
┌──────▼─────────┐   ┌──────────────▼───────────┐   ┌──────▼──────────┐
│ audio/         │   │ core/VoicePipeline       │   │ выход            │
│  QtAudioCapture│   │  vad/SileroVad           │   │  commands/       │
│  Agc           │   │  asr/IRecognizer         │   │   CommandParser  │
│                │   │  text/TextPostProcessor  │   │  output/         │
└────────────────┘   └──────────────────────────┘   │   ITextInjector  │
                                                    └─────────────────┘
┌─────────────────────────────────────────────────────────────────────┐
│ Конфигурация: config/ConfigManager, config/AsrProfile,              │
│               config/HotwordsManager                                │
└─────────────────────────────────────────────────────────────────────┘
```

Зависимости направлены сверху вниз. Ядро не знает о трее и D-Bus; UI и адаптер
D-Bus знают только публичное API контроллера.

## Цепочка прохождения звука

```
QtAudioCapture::audioDataReady(QByteArray, int)
  → ApplicationController::onAudioDataReady      буферизация по 1600 байт (50 мс @ 16 кГц)
  → Agc::process                                 усиление + шумовой порог
  → VoicePipeline::processAudio                  только если mode != Off
  → SileroVad::processAudio                      Silero VAD (sherpa-onnx VoiceActivityDetector)
  → SileroVad::speechSegmentReady(pcm16, 16000)  ЦЕЛЫЙ речевой сегмент
  → IRecognizer::acceptWaveform + finalResult    одно декодирование на фразу
  → TextPostProcessor::process                   «точка» → «.», заглавные, точка в конце
```

Дальше результат расходится двумя сигналами:

```
VoicePipeline::rawTextRecognized(raw)   → CommandParser::parse(raw)
                                          совпало со словарём целиком:
                                            → executeCommand(), m_skipNextText = true
VoicePipeline::textReady(text)          → emit textRecognized(text)
                                          m_skipNextText → пропустить вставку
                                          Mode::Dictation  → ITextInjector::typeText(text)
                                          Mode::Edit       → только команды
                                          Mode::Spellcheck → TODO (AspellChecker пустой)
```

### Почему команда ищется по сырому тексту

`TextPostProcessor` превращает слова «точка», «новая строка», «пробел» в знаки.
Из готового текста команда «новая строка» уже не соберётся. Поэтому:

* `rawTextRecognized` — для `CommandParser`;
* `textReady` — для вставки.

Порядок сигналов гарантирован: `VoicePipeline::onSpeechSegment()` отправляет
`rawTextRecognized` раньше `textReady`, соединения прямые (один поток).

### Почему VAD отдаёт сегменты, а не поток

Нестриминговые модели (zipformer-ru, GigaAM, Whisper) дают заметно лучший результат
на целой фразе, чем на склейке кусков по 32 мс. `SileroVad` поэтому:

1. шлёт `speechStarted()` / `speechEnded()` по переходам `IsDetected()` — для UI;
2. выгребает готовые сегменты через `Front()` / `Pop()` и шлёт `speechSegmentReady()`.

Выгребать обязательно: если сегменты не забирать, очередь внутри
`VoiceActivityDetector` растёт без ограничения.

## Классы

| Класс | Интерфейс | Реализации |
|---|---|---|
| Захват звука | `IAudioCapture` | `QtAudioCapture` |
| VAD | `IVad` | `SileroVad` |
| ASR | `IRecognizer` | `TransducerRecognizer`, `NemoCtcRecognizer`, `WhisperRecognizer` |
| Ввод текста | `ITextInjector` | `XdotoolInjector` |
| Правописание | `ISpellChecker` | `HunspellChecker` (hunspell, C API) |

Конкретную реализацию ASR выбирает `RecognizerFactory::create(AsrProfile)`;
остальное приложение знает только `IRecognizer`.

### Как добавить свой движок ASR

1. Реализовать `IRecognizer` (`initialize`, `acceptWaveform`, `finalResult`,
   `reset`, `setHotwords`, `isLoaded`).
2. Добавить значение в `AsrProfile::Engine` и ветку в `RecognizerFactory::create()`.
3. Добавить профиль в `settings.ini`.

Контроллер, пайплайн и UI при этом не меняются.

### Как добавить способ ввода текста (Wayland/TTY)

Реализовать `ITextInjector` поверх `ydotool` и выбирать его в
`ApplicationController` по значению `[output] method`. Интерфейс уже содержит
`backendName()` для логов и настроек.

## Владение объектами

Все компоненты — `std::unique_ptr` **без** Qt-родителя:

```cpp
m_pipeline = std::make_unique<VoicePipeline>(VoicePipeline::loadSettings(*m_config));
```

QObject-родителя намеренно не передаём. Иначе объект оказывается одновременно
в списке детей родителя и под `unique_ptr`. Порядок деструкции (сначала члены,
потом база `QObject`) делает это безопасным, но полагаться на такой тонкий
момент не стоит — двойное владение легко превращается в double free при
первом же рефакторинге.

## Проверка правописания

`HunspellChecker` реализует `ISpellChecker` и создаётся в конструкторе
`ApplicationController`, если `[spellcheck] enabled=true`. Отсутствие hunspell
или словаря **не мешает** остальному: контроллер логирует причину и продолжает
работать без режима `Spellcheck`.

В режиме `Spellcheck` текст вставляется как при диктовке, а затем проверяется:

```
onTextReady(text) → injectText(text)
                  → runSpellcheck(text) → check(text)  → слова с ошибками
                                        → suggest(word) → варианты
                                        → emit spellcheckFinished(text, errors)
                                          (трей показывает уведомление)
```

Числа («2026») ошибками не считаются: словарь их не знает, а в диктовке они
встречаются постоянно. Слова разбираются регулярным выражением `[^\W\d_]+`
с `UseUnicodePropertiesOption`, поэтому «ё», дефисы и пунктуация обрабатываются
корректно.

`HunspellChecker` намеренно использует **C API** hunspell (`char*`), а не
`hunspell.hxx` (`std::string`): C++ API зависит от `_GLIBCXX_USE_CXX11_ABI`
и ломает линковку при сборке со старым ABI.

## Потоки

Один поток (GUI). Аудио приходит из `QAudioInput` через `QIODevice::readyRead`
в тот же поток; VAD и ASR считаются синхронно внутри слота. Для Celeron это
осознанный выбор: RTF zipformer-ru ≈ 0.06, то есть декодирование фразы в разы
быстрее её длительности, и выносить ASR в отдельный поток пока незачем.

Если понадобится стриминговое распознавание с частичными результатами,
декодирование придётся вынести в worker-поток, а обмен сделать через
`Qt::QueuedConnection`.

## Сборка

`src/CMakeLists.txt` собирает статическую библиотеку `voice-assistant-core`
из всех источников, кроме `main.cpp`, и исполняемый файл `voice-assistant`
из `main.cpp` + ресурсов. Тесты линкуются с `voice-assistant-core`, поэтому
гоняют тот же код, что и приложение, без GUI и микрофона.

Заголовки добавлены в список источников цели обязательно: `IVad`, `IRecognizer`,
`IAudioCapture` — header-only классы с `Q_OBJECT`, и без них AUTOMOC не создаёт
`moc_IVad.cpp`. Подробности — в [sherpa-onnx-notes.md](sherpa-onnx-notes.md).
