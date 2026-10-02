# Интеграция в ApplicationController — РЕАЛИЗОВАНО

Этот документ был планом. План выполнен, ниже — что реально в коде (итерация 5).
Подробный разбор и таблицы замеров — в корневом `README.md`, раздел 0.

## Цепочка

```
QtAudioCapture --audioDataReady(QByteArray,int)--> ApplicationController::onAudioDataReady
    -> буфер по 1600 байт (50 мс @ 16 кГц)
    -> Agc::process()
    -> VoicePipeline::processAudio()          (только если mode != Off)
         -> SileroVad::processAudio()          Silero VAD через sherpa-onnx
         -> SileroVad::speechSegmentReady()    целый речевой сегмент, PCM16 16 кГц
         -> IRecognizer (TransducerRecognizer / NemoCtcRecognizer / WhisperRecognizer)
         -> TextPostProcessor                  «точка» -> «.», заглавные, точка в конце
    -> VoicePipeline::rawTextRecognized(raw)   -> CommandParser::parse(raw)
                                                 совпало целиком -> executeCommand(),
                                                 m_skipNextText = true
    -> VoicePipeline::textReady(text)          -> emit textRecognized(text)
                                                 если m_skipNextText -> пропуск
                                                 Mode::Dictation -> ITextInjector::typeText(text)
                                                 Mode::Edit      -> только команды
                                                 Mode::Spellcheck-> TODO (AspellChecker пуст)
```

## Порядок сигналов важен

`VoicePipeline::onSpeechSegment()` сначала шлёт `rawTextRecognized`, затем `textReady`
(соединения прямые, один поток). Команда ищется по сырому тексту, потому что
`TextPostProcessor` съедает слова «точка», «новая строка», «пробел», превращая их
в знаки, — из готового текста команда уже не соберётся.

## Компоненты контроллера

| Поле | Тип | Роль |
|---|---|---|
| `m_config` | `ConfigManager` | settings.ini: профили ASR, VAD, вывод, команды |
| `m_audioCapture` | `IAudioCapture` | Qt-захват, 16 кГц моно int16 |
| `m_agc` | `Agc` | усиление + шумовой порог |
| `m_pipeline` | `VoicePipeline` | VAD -> ASR -> постобработка |
| `m_injector` | `ITextInjector` | `XdotoolInjector`: буфер обмена или `xdotool type` |
| `m_commands` | `CommandParser` | словарь из 26 команд + свой файл |
| `m_hotwords` | `HotwordsManager` | `commands_hotwords.txt` + `user_hotwords.txt` |

Владение: все поля — `unique_ptr` без Qt-родителя. Родителя QObject намеренно не
передаём, чтобы объект не оказался одновременно в списке детей и под `unique_ptr`.

## Публичное API контроллера

```cpp
Mode mode() const;
void startRecording();               // mode = Dictation
void stopRecording();                // flush() пайплайна, затем mode = Off
void setMode(Mode);

QStringList asrProfiles() const;     // из [asr] profiles=
QString     activeAsrProfile() const;
bool        switchAsrProfile(const QString& name);   // выгрузка старой модели + загрузка новой
void        reloadHotwords();                        // используется и из D-Bus

signals: modeChanged(Mode), errorOccurred(QString),
         textRecognized(QString), commandExecuted(QString);
```

`DBusInterface` (сервис `org.voiceassistant.App`, объект `/org/voiceassistant/App`)
использует ровно это API; `reloadHotwords()` и сигналы `textRecognized`/`errorOccurred`
больше не заглушки.

## Режимы и команды

* Команды смены режима («режим редактирования», «выключить») работают **всегда**.
* Команды правки («удали слово», «новая строка», «пробел», одиночные знаки) в режиме
  диктовки по умолчанию игнорируются: `[commands] editing_in_dictation=false`.
  Иначе продиктованное «удали слово» уничтожало бы само себя.
* Действия: `DeleteWord` -> `ctrl+BackSpace`, `DeleteLine` -> `shift+Home` + `BackSpace`,
  `NewLine` -> `Return`, `Space` -> `space`, `Punctuation` -> `typeText(символ)`.

## Проверка без железа

```bash
VOICE_ASSISTANT_DRYRUN=1 ./src/voice-assistant    # инжектор логирует вместо отправки
ctest --output-on-failure                          # 2 теста, 23 проверки
./tools/vad-asr-test запись.wav --config ~/.config/voice-assistant/settings.ini
```
