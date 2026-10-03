# Конфигурация

Файл: `~/.config/voice-assistant/settings.ini` (формат INI, читается `QSettings`).
Шаблон с комментариями лежит в `config/settings.ini` репозитория.

Рядом кладутся два файла подсказок:

```
~/.config/voice-assistant/commands_hotwords.txt   диктантные команды (уже заполнен)
~/.config/voice-assistant/user_hotwords.txt       свои слова: имена, термины, названия
~/.config/voice-assistant/commands.txt            свои команды (необязательно)
```

## Пути к моделям

`ConfigManager::resolvePath()` понимает три формы, поэтому конфиг переносим
между машинами:

| Запись | Во что раскрывается |
|---|---|
| `sherpa-onnx-small-zipformer-ru-2024-09-18/encoder.int8.onnx` | `~/.voice_models/…` |
| `~/models/zipformer-ru/encoder.int8.onnx` | от домашнего каталога |
| `/opt/voice-models/encoder.int8.onnx` | как есть |

Корневой каталог моделей — `~/.voice_models` (`ConfigManager::modelsPath()`).

## `[asr]` — профили распознавания

```ini
[asr]
active=zipformer-ru
profiles=zipformer-ru,gigaam-v3,gigaam-v3-ctc,whisper-base
num_threads=2          ; значение по умолчанию для профилей, где num_threads не задан
hotwords_score=2.0     ; сила бустинга подсказок
```

`active` — какой профиль грузить при старте. `profiles` — список для переключения
(трей, D-Bus, `ApplicationController::switchAsrProfile()`).

⚠️ `profiles` перечисляются через запятую, и `QSettings` возвращает такое значение
как `QStringList`, а не строку. `ConfigManager::asrProfileNames()` это учитывает;
если читать значение через `.toString()` напрямую, получится пустая строка.

## `[asr_<имя>]` — описание профиля

Общие ключи:

| Ключ | Значения | Комментарий |
|---|---|---|
| `engine` | `transducer`, `nemo-ctc`, `whisper` | определяет, какие пути обязательны |
| `tokens` | путь | словарь токенов модели |
| `num_threads` | 1–4 | для Celeron ставьте 1–2 |
| `decoding_method` | `modified_beam_search`, `greedy_search` | hotwords работают только в `modified_beam_search` |
| `max_active_paths` | 4 | ширина луча |
| `hotwords_score` | 2.0 | переопределяет общее значение |
| `commands_hotwords_score` | 2.0 | скор для фраз команд из словаря |
| `punctuation_hotwords_score` | 2.0 | скор для диктантных знаков; замер показал, что ниже 1.75 модель их теряет |
| `debug` | `false` | лог sherpa-onnx |

По движкам:

| `engine` | Обязательные ключи |
|---|---|
| `transducer` | `encoder`, `decoder`, `joiner`, `tokens` |
| `nemo-ctc` | `model`, `tokens` |
| `whisper` | `encoder`, `decoder`, `tokens`, необязательно `language` |

Дополнительно для BPE-моделей (zipformer-ru), чтобы подсказки можно было писать
обычными словами:

| Ключ | Значение |
|---|---|
| `bpe_vocab` | путь к словарю ssentencepiece (`tools/gen_bpe_vocab.py`) |
| `modeling_unit` | `bpe`; подставляется автоматически, если задан `bpe_vocab` |

Профиль не пройдет проверку, если хотя бы один обязательный файл отсутствует:
`AsrProfile::isValid()` возвращает подробную ошибку, она уходит в
`errorOccurred` и в лог — приложение не падает молча.

### Примеры профилей

```ini
[asr_zipformer-ru]
engine=transducer
encoder=sherpa-onnx-small-zipformer-ru-2024-09-18/encoder.int8.onnx
decoder=sherpa-onnx-small-zipformer-ru-2024-09-18/decoder.int8.onnx
joiner=sherpa-onnx-small-zipformer-ru-2024-09-18/joiner.int8.onnx
tokens=sherpa-onnx-small-zipformer-ru-2024-09-18/tokens.txt
bpe_vocab=sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.vocab
modeling_unit=bpe
decoding_method=modified_beam_search
num_threads=2

[asr_gigaam-v3]
engine=transducer
encoder=sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/encoder.int8.onnx
decoder=sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/decoder.onnx
joiner=sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/joiner.onnx
tokens=sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16/tokens.txt
decoding_method=greedy_search          ; GigaAM тяжёлый, луч тут дорогой
num_threads=2

[asr_whisper-base]
engine=whisper
encoder=sherpa-onnx-whisper-base/base-encoder.int8.onnx
decoder=sherpa-onnx-whisper-base/base-decoder.int8.onnx
tokens=sherpa-onnx-whisper-base/base-tokens.txt
language=ru
```

## `[vad]`

| Ключ | По умолчанию | Комментарий |
|---|---|---|
| `model` | `silero_vad.onnx` | путь (правила те же) |
| `threshold` | `0.5` | порог «речь / не речь» |
| `min_silence_duration` | `0.5` | секунды тишины, закрывающие сегмент. Больше — фразы длиннее и реже режутся |
| `min_speech_duration` | `0.25` | короче — не речь |
| `window_size` | `512` | сэмплов на окно (32 мс @ 16 кГц), не менять без нужды |
| `num_threads` | `1` | VAD лёгкий, одного потока хватает |
| `buffer_seconds` | `60` | размер кольцевого буфера детектора |

VAD в sherpa-onnx **не ресемплит**: `SileroVad` принимает только 16 кГц моно
int16 и сообщает об ошибке иначе. `QtAudioCapture` запрашивает ровно этот формат.

## `[text]`

| Ключ | По умолчанию | Что делает |
|---|---|---|
| `auto_punctuate` | `true` | заглавная в начале предложения, точка в конце |
| `voice_punctuation` | `true` | «точка» → `.`, «запятая» → `,` и т. д. |

Подробности и таблица слов — в
[hotwords-and-punctuation.md](hotwords-and-punctuation.md).

## `[output]`

| Ключ | По умолчанию | Комментарий |
|---|---|---|
| `method` | `auto` | `auto` — буфер обмена, если есть `xclip`/`xsel`/`wl-copy`, иначе `xdotool type`; `clipboard` — только буфер; `xdotool` — только посимвольный ввод |
| `typing_delay_ms` | `12` | задержка между символами в режиме `xdotool type` |
| `space_between_segments` | `true` | добавлять пробел между фразами: VAD отдаёт речь кусками, каждый вставляется отдельным вызовом |
| `preserve_clipboard` | `true` | запомнить прежний текст буфера и вернуть его после вставки |
| `clipboard_restore_ms` | `1000` | через сколько миллисекунд возвращать буфер |

Вставка через буфер затирает его содержимое — это неизбежная плата за независимость
от раскладки. При `preserve_clipboard=true` прежний **текст** возвращается обратно
через `clipboard_restore_ms`. Картинки и прочие mime-типы не сохраняются.
Задержку стоит увеличить, если целевое приложение медленное и иногда вставляет
уже восстановленное содержимое.

Буфер обмена предпочтительнее: `xdotool type` для кириллицы требует русской
раскладки в момент ввода (иначе вместо «привет» уедет «ghbdtn») и медленнее.
Обратная сторона — затирается содержимое буфера обмена.

## `[commands]`

| Ключ | По умолчанию | Комментарий |
|---|---|---|
| `editing_in_dictation` | `false` | выполнять ли команды правки в режиме диктовки |
| `file` | `~/.config/voice-assistant/commands.txt` | свой файл команд |

Формат своего файла — `фраза = тип[:аргумент]`, по одной команде в строке,
`#` — комментарий:

```
# фраза            = тип[:аргумент]
открой консоль     = new-line
шумный режим       = set-mode:edit
кавычка            = punctuation:"
```

Типы: `set-mode:{dictation|edit|spellcheck|off}`, `delete-word`, `delete-line`,
`new-line`, `space`, `punctuation:<символ>`. Битые строки пропускаются с
предупреждением в лог, а не роняют загрузку.

## `[spellcheck]`

| Ключ | По умолчанию | Комментарий |
|---|---|---|
| `enabled` | `true` | включать ли проверку правописания вообще |
| `lang` | `ru_RU` | имя словаря: ищутся `<lang>.aff` и `<lang>.dic` |
| `dictionary_dir` | пусто | каталог словаря; пусто = стандартные пути |
| `max_suggestions` | `5` | сколько вариантов исправления предлагать |

Стандартные пути поиска: `/usr/share/hunspell`, `/usr/share/myspell`,
`/usr/local/share/hunspell`, `~/.hunspell`, `~/.local/share/hunspell`.

Словарь обязан быть в **UTF-8** — `HunspellChecker` проверяет кодировку через
`Hunspell_get_dic_encoding()` и отказывается работать с KOI8-R/ISO8859-5,
поскольку текст из ASR приходит в UTF-8 и сравнивается побайтово.

Отсутствие hunspell не ломает сборку: зависимость детектируется pkg-config,
без неё `initialize()` возвращает false с внятным сообщением.

## `[audio]`

| Ключ | По умолчанию | Комментарий |
|---|---|---|
| `debug_log` | `false` | лог RMS/gain по 20 строк в секунду. На слабом CPU держать выключенным |

## Переменные окружения

| Переменная | Действие |
|---|---|
| `VOICE_ASSISTANT_DRYRUN=1` | `XdotoolInjector` логирует вместо отправки клавиш — для тестов и отладки без X-сервера |

## Аргументы командной строки

| Аргумент | Действие |
|---|---|
| `--check` | диагностика конфигурации и моделей, без GUI и микрофона; код возврата 0 — всё на месте, 1 — есть проблемы |
| `--config <путь>` | использовать указанный `settings.ini` вместо `~/.config/voice-assistant/settings.ini` (работает и с `--check`, и с `tools/vad-asr-test`) |

## Смена профиля в рантайме

```cpp
controller.switchAsrProfile(QStringLiteral("gigaam-v3"));
```

Старая модель выгружается (`unique_ptr` сбрасывается до создания новой), ОЗУ
освобождается. Переинициализация стоит 1.2–5.1 с, поэтому делать её стоит
по явному действию пользователя, а не автоматически.
