# Заметки по sherpa-onnx

Проверено на заголовках, совпадающих байт в байт с установленными в системе:
`cxx-api.h` — 67 563 Б (66.0 KB), `c-api.h` — 169 199 Б (165.2 KB).

## Совместимость версий

Код собирается и работает как против master, так и против релиза v1.13.8.
Различия в `cxx-api.h` между ними — 16 строк и наших типов они не касаются:

* добавлен `OfflineCtcFstDecoderConfig` и одноимённое поле в конец
  `OfflineRecognizerConfig`;
* у `SpeakerEmbeddingManager` появились `Dim()` и `GetEmbedding()`.

Структуры `VadModelConfig`, `SileroVadModelConfig`, `TenVadModelConfig`,
`SpeechSegment`, `OfflineTransducerModelConfig`, `OfflineNemoEncDecCtcModelConfig`,
`OfflineWhisperModelConfig`, `OfflineSenseVoiceModelConfig`, `FeatureConfig`
в двух версиях идентичны.

⚠️ Поле в **конце** `OfflineRecognizerConfig` меняет размер структуры, поэтому
смешивать заголовки одной версии с библиотеками другой нельзя — см. раздел про ABI.

## Реальный API (то, что используется в проекте)

Пространство имён — `sherpa_onnx::cxx`, заголовок — `<sherpa-onnx/c-api/cxx-api.h>`.

### VAD

```cpp
VadModelConfig config;                       // НЕ "VoiceActivityDetectorConfig"
config.silero_vad.model = "/path/silero_vad.onnx";
config.silero_vad.threshold = 0.5f;
config.silero_vad.min_silence_duration = 0.5f;
config.silero_vad.min_speech_duration = 0.25f;
config.silero_vad.max_speech_duration = 20.0f;
config.silero_vad.window_size = 512;
config.sample_rate = 16000;                  // только 16 кГц, VAD сам не ресемплит
config.num_threads = 1;
config.provider = "cpu";

auto vad = VoiceActivityDetector::Create(config, /*buffer_size_in_seconds=*/60.0f);
if (vad.Get() == nullptr) { /* ошибка, исключений не будет */ }

vad.AcceptWaveform(samples, n);              // длина чанка произвольная
bool speaking = vad.IsDetected();            // НЕ Detected()
while (!vad.IsEmpty()) {
    SpeechSegment seg = vad.Front();         // seg.start, seg.samples
    vad.Pop();                               // обязательно, иначе очередь растёт
}
vad.Flush();                                 // вытолкнуть хвост (push-to-talk)
vad.Clear();  vad.Reset();
```

`AcceptWaveform` принимает чанки произвольной длины: внутри
`VoiceActivityDetectorImpl` копит сэмплы и режет по `window_size` сам.

### Offline ASR

```cpp
OfflineRecognizerConfig config;
config.feat_config.sample_rate = 16000;
config.feat_config.feature_dim = 80;

// transducer (zipformer-ru, GigaAM RNN-T)
config.model_config.transducer.encoder = "...";
config.model_config.transducer.decoder = "...";
config.model_config.transducer.joiner  = "...";
// nemo-ctc (GigaAM CTC)
config.model_config.nemo_ctc.model     = "...";
// whisper
config.model_config.whisper.encoder    = "...";
config.model_config.whisper.decoder    = "...";
config.model_config.whisper.language   = "ru";

config.model_config.tokens      = "...";
config.model_config.num_threads = 2;
config.model_config.provider    = "cpu";
config.model_config.modeling_unit = "bpe";   // для hotwords у BPE-моделей
config.model_config.bpe_vocab     = "...";   // словарь ssentencepiece

config.decoding_method  = "modified_beam_search";  // или "greedy_search"
config.max_active_paths = 4;
config.hotwords_score   = 2.0f;

auto rec = OfflineRecognizer::Create(config);      // НЕ make_unique<OfflineRecognizer>(config)
if (rec.Get() == nullptr) { /* ошибка */ }

auto stream = rec.CreateStream();                  // или CreateStream(hotwords)
stream.AcceptWaveform(sampleRate, data, n);        // ресемплит сам при нужде
rec.Decode(&stream);                               // НЕ rec.DecodeStream(&stream)
OfflineRecognizerResult r = rec.GetResult(&stream);// НЕ stream.GetResult()
QString text = QString::fromStdString(r.text);     // r.lang, r.emotion, r.event
```

### Чего в API нет (частые ошибки)

| Не существует | Есть |
|---|---|
| `VoiceActivityDetectorConfig` | `VadModelConfig` |
| `config.model_config.silero_vad` | `config.silero_vad` |
| конструктор `VoiceActivityDetector(config)` | `VoiceActivityDetector::Create(config, seconds)` |
| конструктор `OfflineRecognizer(config)` | `OfflineRecognizer::Create(config)` |
| `vad.Detected()`, `vad.Empty()` | `vad.IsDetected()`, `vad.IsEmpty()` |
| `rec.DecodeStream(&s)` | `rec.Decode(&s)` |
| `s.GetResult()` | `rec.GetResult(&s)` |

## Обработка ошибок: исключений нет

C API sherpa-onnx не бросает исключения. При ошибке `Create()` возвращает
обёртку с нулевым хендлом, а сообщение печатается в stderr. Поэтому
`try/catch (const std::exception&)` вокруг `Create` бесполезен — проверять
нужно `Get()`:

```cpp
auto rec = OfflineRecognizer::Create(config);
if (rec.Get() == nullptr) {
    emit errorOccurred(QStringLiteral("OfflineRecognizer::Create failed"));
    return false;
}
```

Все три наши реализации (`TransducerRecognizer`, `NemoCtcRecognizer`,
`WhisperRecognizer`) и `SileroVad` проверяют и существование файлов моделей
до вызова `Create`, чтобы ошибка была внятной: «модель не найдена: путь»,
а не безмолвный `nullptr`.

## Формат сэмплов

`OfflineStream::AcceptWaveform` ожидает `float` в диапазоне `[-1, 1]`
(`normalize_samples = true` выставляется в конструкторе `OfflineStream::Impl`).
Наше преобразование PCM16 → float: `sample / 32768.0f`.

Ресемплинг делать не нужно: если `sample_rate` отличается от
`feat_config.sample_rate`, стрим сам создаёт `LinearResample`
(и печатает в лог «Creating a resampler» — это не ошибка).

## ONNX Runtime отдельно не нужен

* `libonnxruntime.so` линкуется внутри `libsherpa-onnx-c-api.so`;
* заголовок `sherpa-onnx/c-api/onnxruntime_cxx_api.h` поставляется вместе
  с sherpa-onnx;
* свой `Ort::Env` / `Ort::Session` для Silero VAD писать не нужно — VAD уже
  есть в sherpa-onnx (`VoiceActivityDetector`).

В линковке достаточно двух библиотек:

```cmake
target_link_libraries(voice-assistant-core PUBLIC
    ${SHERPA_ONNX_CXX_LIBRARY}   # libsherpa-onnx-cxx-api.so
    ${SHERPA_ONNX_C_LIBRARY})    # libsherpa-onnx-c-api.so
```

Если бинарник не находит `.so` при запуске, в корневом `CMakeLists.txt` уже
прописан rpath на `/usr/local/lib`; альтернатива — `sudo ldconfig`.

## Грабля 1: ABI libstdc++ (segfault без ошибок компиляции)

Официальные **предсобранные** sherpa-onnx (linux-x64 shared) собраны со старым
ABI libstdc++. Доказательство — манглинг экспортируемого символа:

```
$ nm -D --defined-only libsherpa-onnx-cxx-api.so | grep 12CreateStream
_ZNK11sherpa_onnx3cxx17OfflineRecognizer12CreateStreamERKSs
                                                     ^^^ Ss = std::string старого ABI
```

Для нового ABI там было бы `NSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE`.

Если такой библиотеке передать `cxx::VadModelConfig`, собранный с новым ABI,
размеры не совпадут (`sizeof` 160 против 88: `std::string` — 32 байта против 8),
и библиотека прочитает указатель `ten_vad.model` из наших float'ов:

```
Program received signal SIGSEGV
#0  vpcmpeqb (%rdi),%ymm16,%k0    # strlen в libc; rdi = 0x3f0000003f000000 = два float 0.5f
#1  SherpaOnnxCreateVoiceActivityDetector ()
#2  sherpa_onnx::cxx::VoiceActivityDetector::Create ()
#3  SileroVad::initialize ()
```

Компилируется это **без единой ошибки и предупреждения** — падает только в рантайме.

Проверка и лечение:

```bash
nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs
#   >0 -> старый ABI: cmake .. -DSHERPA_ONNX_OLD_CXX_ABI=ON
#    0 -> новый ABI: ничего не нужно
```

Опция `SHERPA_ONNX_OLD_CXX_ABI` добавляет `-D_GLIBCXX_USE_CXX11_ABI=0`
ко всему проекту. С Qt5 это сочетается: Qt в публичном API использует `QString`
и `QByteArray`, а не `std::string`, поэтому границы ABI не пересекаются.
Проверено: весь проект и все тесты собираются и работают в обоих вариантах.

Надёжнее всего собрать sherpa-onnx из исходников тем же компилятором, что
и проект, — тогда вопрос не возникает.

## Грабля 2: header-only классы с Q_OBJECT и AUTOMOC

`IVad`, `IRecognizer`, `IAudioCapture` — header-only классы с `Q_OBJECT`
и сигналами. Если в списке источников цели нет заголовков, AUTOMOC не создаёт
`moc_IVad.cpp`, и линковка падает:

```
undefined reference to `IVad::speechStarted()'
undefined reference to `IVad::errorOccurred(QString const&)'
undefined reference to `vtable for IVad'
```

Лечение — добавить `*.h` в источники цели (`src/CMakeLists.txt`,
`tools/CMakeLists.txt`):

```cmake
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "*.h")
add_library(voice-assistant-core STATIC ${SOURCES} ${HEADERS})
```

Попытка объявить сигналы повторно в производном классе с `override` не работает
и не нужна: сигналы Qt не виртуальные, они объявлены в интерфейсе и наследуются.

## Грабля 3: CMake молча принимает несуществующую цель

`target_link_libraries(test PRIVATE voice-assistant-core)` с несуществующей целью
не вызывает ошибку конфигурации — CMake считает имя обычной библиотекой
(`-lvoice-assistant-core`). Падает позже и неочевидно: сначала на отсутствии
include-путей. Цель `voice-assistant-core` введена в `src/CMakeLists.txt`
именно поэтому.

## Полезные команды

```bash
# версия установленной библиотеки
strings /usr/local/lib/libsherpa-onnx-c-api.so | grep -m1 -E '^1\.[0-9]+\.[0-9]+$'

# размеры заголовков (сверка с документированными здесь)
wc -c /usr/local/include/sherpa-onnx/c-api/{c-api.h,cxx-api.h}

# официальный декодер для сравнения с нашим результатом
sherpa-onnx-offline --encoder=… --decoder=… --joiner=… --tokens=… запись.wav

# официальный VAD для проверки silero_vad.onnx
sherpa-onnx-vad --silero-vad-model=… in.wav out.wav
```
