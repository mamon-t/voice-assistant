# voice-assistant

Offline voice assistant for Linux: dictation into any input field, voice editing commands, and spellchecking. Recognition runs locally — no internet, no clouds, no sending audio anywhere. Designed for low-end hardware (Celeron, 4–16 GB RAM).

![README russian](README_RU.md) 

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)
![Qt 5.13+](https://img.shields.io/badge/Qt-5.13%2B-41CD52.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![Status](https://img.shields.io/badge/status-alpha-orange.svg)

---

## Features

* **Dictation into any application** — text is inserted via clipboard + `Ctrl+V`
  or via `xdotool type`; works in browsers, editors, and terminals.
  Previous clipboard contents are remembered and restored
  (`[output] preserve_clipboard`).
* **Fully offline** — VAD (Silero) and ASR (zipformer-ru / GigaAM / Whisper)
  run locally via [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx).
* **Voice commands** — switch mode, delete word/line, new line,
  punctuation. The dictionary can be extended with your own file.
* **Voice punctuation** — “period”, “comma”, “question mark”, “paragraph”
  become marks; capitalization and the final period are placed automatically.
* **User hotwords** — rare words, names, and terms from a file
  improve recognition accuracy: for transducer models, true
  contextual boosting works rather than text substitution.
* **Spellchecking** — `Spellcheck` mode checks dictated text via
  hunspell with a Russian dictionary and shows correction options in a notification.
* **Notes to file** — a second output target: recognized text is appended to
  `~/.local/share/voice-assistant/notes/YYYY-MM-DD.md` instead of the active window.
  Neither focus, nor keyboard, nor clipboard is involved, so notes for the file
  you are working with can be dictated while continuing to type by hand. Switching —
  by voice (“заметка” / “в редактор”), from the tray, or via D-Bus.
* **Audio file transcription** — the command “разбери файл” or the tray item
  “Transcribe audio file…” opens the system file picker for a recording; the transcript
  is inserted into the window that was active before the dialog (or appended to notes).
  Formats: WAV (PCM 8/16/24/32, float, telephone A-law/μ-law), RAW without a container
  (the dialog asks for parameters), MP3/OGG/FLAC/M4A/AMR via the system decoder.
  From the console — `--transcribe` with ASR profile selection.
* **Dictation does not block the keyboard** — `evdev` only listens to events
  (`[hotkey] grab=false` by default), and `EVIOCGRAB` is additionally guarded:
  a device resembling a full keyboard is not grabbed, even if the config has
  `grab=true`, until it is explicitly named in `grab_devices`.
* **Typing during dictation** — `[output] pin_window` binds insertion to the window
  active at the start of recording, and `[audio] typing_guard_ms` mutes the microphone
  while typing so keystrokes do not turn into words. The assistant’s own windows
  (tray menu) do not become pinned, and each insertion is logged with address and
  method — “text went somewhere unknown” is no longer diagnosed by eye.
* **Global microphone hotkey** — `evdev`, works in X11, Wayland, and bare TTY;
  two modes: push-to-talk (hold the key) and toggle (press/release).
* **Microphone check** — recording the path to WAV from the tray, via D-Bus, or from
  the console (`--record 15 ~/mic.wav`): the same file is then run through all ASR
  profiles, which separates microphone and AGC problems from model problems.
* **Switchable ASR profiles** — the model is changed by one line in the config or
  on the fly from D-Bus/tray: fast zipformer-ru for silence, GigaAM v3 for noise.
* **Model download wizard** — on first launch the application itself will offer to
  download models (list with size and description, sha256, resume on interruption,
  atomic unpacking); from the console — `--download-model`.
* **Ready-made .deb/.rpm packages** — sherpa-onnx/onnxruntime/hunspell come inside
  the package, clean removal (see “Installation”).
* **External control via D-Bus** — service `org.voiceassistant.App`.

## Requirements

| Component              | Version                                                         | Purpose                                                                                                                                  |
| ---------------------- | --------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| Linux, X11             | —                                                               | for Wayland/TTY, `ydotool` will be required (planned)                                                                                    |
| g++ / clang            | C++17                                                           | build                                                                                                                                    |
| CMake                  | 3.16+                                                           | build                                                                                                                                    |
| Qt                     | 5.13+ (Core, Widgets, Multimedia, DBus, Test)                   | framework, sound, tray, D-Bus                                                                                                            |
| sherpa-onnx            | with C++ API (`cxx-api.h`)                                      | VAD and ASR                                                                                                                              |
| xdotool, xclip         | any                                                             | text insertion                                                                                                                           |
| access to `/dev/input` | group `input`                                                   | global hotkey via evdev                                                                                                                  |
| hunspell + dictionary  | libhunspell-dev, hunspell-ru                                    | spellchecking (optional)                                                                                                                 |
| gstreamer plugins      | libqt5multimedia5-plugins, gstreamer1.0-plugins-good/ugly/libav | MP3/OGG/FLAC/M4A in audio file transcription (optional; WAV and RAW work without them). On Linux Mint they are usually already installed |
| Free space             | ~300 MB                                                         | models (VAD 0.6 MB + ASR 110–208 MB)                                                                                                     |

## Installation

### 0. Ready-made packages (recommended)

Download the `.deb` (Debian/Ubuntu/Mint) or `.rpm` (Fedora/openSUSE) from the
[Releases](https://github.com/mamon-t/voice-assistant/releases) page:

```bash
sudo apt install ./voice-assistant_0.9.0_amd64.deb    # Debian/Ubuntu/Mint
sudo dnf install ./voice-assistant-0.9.0-1.x86_64.rpm # Fedora
```

The package is self-contained: sherpa-onnx, onnxruntime, and hunspell come inside
(`/usr/lib/voice-assistant/`, RPATH `$ORIGIN`); system libraries are not
replaced. On first launch the assistant will offer the **model download wizard**
(~110 MB, sha256 verification, resume on interruption); from the console the same
is done with `voice-assistant --download-model zipformer-ru`. For the global hotkey,
`sudo usermod -aG input $USER` and a relogin will be required (postinst will
remind you about this). Removal: `sudo apt remove voice-assistant` — system files
are removed cleanly; user data (settings, notes, models) remain in the home
directory — for how to remove them, see the end of this section.

### 1. System dependencies

```bash
sudo apt install build-essential cmake \
    qtbase5-dev qtmultimedia5-dev \
    xdotool xclip

# optional, for spellcheck mode:
sudo apt install libhunspell-dev hunspell-ru
```

Without hunspell the project also builds — `Spellcheck` mode will simply report
that support is not enabled (the dependency is detected via pkg-config,
see the `cmake` output).

### 2. sherpa-onnx

It is recommended to build from source with the same compiler as the project — then
the libstdc++ ABI is guaranteed to match:

```bash
git clone https://github.com/k2-fsa/sherpa-onnx.git
cd sherpa-onnx && mkdir build-shared && cd build-shared
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
         -DSHERPA_ONNX_ENABLE_TTS=OFF -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
         -DSHERPA_ONNX_ENABLE_TESTS=OFF
make -j$(nproc) && sudo make install && sudo ldconfig
```

If you install **prebuilt sherpa-onnx binaries**, check the ABI — otherwise the app
will build without errors but crash in `strlen()` when creating the VAD:

```bash
nm -D --defined-only /usr/local/lib/libsherpa-onnx-cxx-api.so | grep -c ERKSs
#   >0  -> library built with old ABI, build the project with
#          cmake .. -DSHERPA_ONNX_OLD_CXX_ABI=ON
#    0  -> normal new ABI, nothing to add
```

Details — in [docs/sherpa-onnx-notes.md](docs/sherpa-onnx-notes.md).

### 3. Models

The main method is the **download wizard in the application itself**: on first launch
without models it opens by itself (or tray → “Download models...”). sha256
verification, resume on interruption, the profile automatically becomes active.
From the console:

```bash
voice-assistant --download-model list            # what is available and what is installed
voice-assistant --download-model zipformer-ru    # VAD will be added automatically
```

Manually (alternative, e.g. without GUI):

```bash
mkdir -p ~/.voice_models && cd ~/.voice_models

# VAD (required, 0.6 MB)
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx

# Default ASR: Russian zipformer (110 MB)
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2
tar xf sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2 && rm sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2

# dictionary for hotwords in ordinary words (needed only for zipformer-ru)
python3 -m pip install --user sentencepiece
python3 <path to project>/tools/gen_bpe_vocab.py \
    ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.model \
    ~/.voice_models/sherpa-onnx-small-zipformer-ru-2024-09-18/bpe.vocab
```

Alternative profiles (GigaAM v3 RNN-T for noisy environments, Whisper for English)
and a comparison of models by accuracy and speed — in [docs/models.md](docs/models.md).

### 4. Config

```bash
mkdir -p ~/.config/voice-assistant
cp config/* ~/.config/voice-assistant/
```

### 5. Build

```bash
git clone https://github.com/mamon-t/voice-assistant.git
cd voice-assistant
mkdir -p build && cd build
cmake ..              # + -DSHERPA_ONNX_OLD_CXX_ABI=ON, if the ABI check returned >0
make -j$(nproc)
ctest --output-on-failure
```

### Complete removal (package and data)

`sudo apt remove voice-assistant` (or `dnf remove`) removes all system
files cleanly. The package does not touch user data during either remove or
purge — that is how distributions work. They are removed with one command:

```bash
rm -rf ~/.config/voice-assistant ~/.local/share/voice-assistant ~/.voice_models
```

## Quick start

```bash
./src/voice-assistant --check      # diagnostics: models, injector, dictionary, hotkey, /dev/input
./src/voice-assistant              # icon in the system tray
./src/voice-assistant --record 15 ~/mic.wav   # record 15 s from the microphone (raw path, no AGC)
./src/voice-assistant --transcribe ~/call.mp3 --profile gigaam-v3   # transcribe a recording (text to stdout)
./src/voice-assistant --note "check AGC"  # append a line to the notes file and exit
./src/voice-assistant --pin-info              # where the text will go: window, WM_CLASS, insertion method
./src/voice-assistant --type "insertion test" --pin-active --delay 3000
                                             # insert text without starting recognition
```

For the global hotkey, access to input devices is required:

```bash
sudo usermod -aG input $USER      # then relogin
./src/voice-assistant --check     # line “/dev/input: N devices, readable: M”
```

Without permissions for `/dev/input`, the application works as usual — modes
are switched from the tray and via D-Bus, the hotkey simply does not start.

`--check` requires neither a microphone nor an X server and prints, for each ASR
profile, all paths marked `OK (size)` or `NO FILE`, and at the end — the number of
problems. Return code 0/1, so it can be run in scripts. If the application has started
but speech is not recognized, start with it.

* Click the icon — start/stop dictation.
* `F8` (default) — push-to-talk: hold while speaking; release — the phrase
  goes to recognition. The key and mode are configured in `[hotkey]`.
* Right click — select mode (dictation / edit / spellcheck),
  “Write to notes file”, “Open notes”, settings, hotwords editor, exit.
* Say ordinary text — it appears in the active input field.
* Say “заметка” — and the text goes to the notes file, not the window: you can
  continue typing by hand. Back — “в редактор”.
* Say “режим редактирования”, then “удали слово”, “новая строка”, etc.
* In “Spellcheck” mode, text is inserted as during dictation, and found
  errors are shown in a notification with options: `прверка → проверка, поверка`.

`--check`, besides models and the injector, checks the dictionary and immediately runs
a control word:

```
Spelling
  engine    : hunspell
  dictionary: /usr/share/hunspell/ru_RU.dic
  control   : "прверка" -> error, options: проверка, поверка, привертка
```

Control from the terminal:

```bash
BUS=org.voiceassistant.App
OBJ=/org/voiceassistant/App
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.getMode
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.setMode "dictation"
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.reloadHotwords
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.setOutputTarget "notes"
gdbus call --session --dest $BUS --object-path $OBJ --method $BUS.notesFile
gdbus monitor --session --dest $BUS      # watch signals textRecognized, noteWritten
```

Checking the pipeline without a microphone or X server:

```bash
VOICE_ASSISTANT_DRYRUN=1 ./src/voice-assistant     # injector logs instead of sending
./tools/vad-asr-test recording.wav --config ~/.config/voice-assistant/settings.ini
```

## Configuration

Everything is in `~/.config/voice-assistant/settings.ini`. ASR profiles are described
by `[asr_<name>]` sections; the active one is selected by the `active=` line in the
`[asr]` section.

```ini
[asr]
active=zipformer-ru
profiles=zipformer-ru,gigaam-v3,gigaam-v3-ctc,whisper-base
```

Model paths can be specified relative to `~/.voice_models`, from home (`~/...`),
or absolutely. The full reference of all keys is in
[docs/configuration.md](docs/configuration.md).

## Voice commands

| Phrase                                                    | Action                                            |
| --------------------------------------------------------- | ------------------------------------------------- |
| «режим диктовки» / «режим ввода»                          | `Mode::Dictation`                                 |
| «режим редактирования» / «режим правки»                   | `Mode::Edit`                                      |
| «режим проверки» / «режим правописания»                   | `Mode::Spellcheck`                                |
| «выключить» / «стоп»                                      | `Mode::Off`                                       |
| «заметка» / «в заметки»                                   | output target — notes file                        |
| «в редактор» / «в окно»                                   | output target — active window                     |
| «разбери файл» / «разбери запись» / «расшифруй аудиофайл» | audio file picker — transcript to window or notes |
| «удали слово» / «сотри слово»                             | `Ctrl+BackSpace`                                  |
| «удали строку»                                            | `Shift+Home`, `BackSpace`                         |
| «новая строка» / «абзац»                                  | `Enter`                                           |
| «пробел»                                                  | space                                             |
| «точка», «запятая», «двоеточие», «тире» …                 | the corresponding punctuation mark                |

Only a phrase matching the dictionary **in full** is considered a command: “привет мир” —
this is text, “удали слово” — a command, and “сделать заметку на полях” — text again.
Editing commands in dictation mode are disabled by default
(`[commands] editing_in_dictation=false`), otherwise the dictated “удали слово”
would eat itself. Mode and output target switching commands always work:
this is routing, not text editing. If the word “заметка” gets in the way in ordinary
speech, voice switching is disabled (`[notes] voice_commands=false`),
and the target remains accessible from the tray and via D-Bus. Custom commands are
added by a file `phrase = type[:argument]` — see [docs/configuration.md](docs/configuration.md).

## Audio file transcription

A phone call recording, a voice message, a WAV from a microphone check —
any audio file is run through the same production path (Silero VAD → ASR →
punctuation) as dictation.

**From the GUI.** Say “разбери файл” during dictation (or choose the
“Transcribe audio file…” item in the tray menu) → the system file picker opens →
after selection, the assistant stops recording, shows progress with a “Cancel”
button, and the finished transcript goes **to the window that was active before
the dialog** (e.g., XED), or to the notes file if the output target is notes. The
window is remembered in advance not by accident: the dialog and progress are the
assistant’s own windows, and after they close, focus is unpredictable. Delivery uses
the same `pin_mode=activate` mechanism: the window is activated, text is typed
with real events, focus is returned.

Speech segments (VAD cuts them by pauses) are joined with a newline:
for a call transcript, each utterance ends up on its own line.

**Formats.**

| Format                                            | How it is read                                                                                                                                                                        |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| WAV                                               | own parser: PCM 8/16/24/32 bit, float32/64, A-law, μ-law, any channels and sample rate                                                                                                |
| RAW (`.raw`, `.pcm`, `.audio`, without extension) | containerless PCM: sample rate/channels/encoding asked by the dialog (default telephone 8000 Hz, mono, s16le), choice remembered in `[transcribe]`                                    |
| MP3, OGG/Opus, FLAC, M4A/AAC, AMR, WMA            | system decoder (QtMultimedia → GStreamer); requires `libqt5multimedia5-plugins` and gstreamer plugins — on Linux Mint they are usually already installed, in .deb added to Recommends |

Everything is downmixed to mono and resampled to 16 kHz.

**From the console** (the model is selected by a profile from the config — the same as
in the GUI; the list of profiles is shown by `--check`):

```bash
voice-assistant --transcribe call.mp3                    # active profile, text to stdout
voice-assistant --transcribe call.mp3 --profile gigaam-v3   # another model (noisy audio)
voice-assistant --transcribe recording.raw --rate 8000 --format alaw --out call.txt
voice-assistant --transcribe voice.wav --notes             # append to notes file
voice-assistant --transcribe voice.wav --insert            # insert into active window
voice-assistant --transcribe call.mp3 --config /path/to/settings.ini   # another config
```

The result is always printed to stdout (convenient for scripts:
`--transcribe call.mp3 > call.txt`), diagnostics and progress go to stderr.
`--profile`/`--config` select the model, `--threads`/`--no-punct`/`--hotwords`
refine the mode. Arbitrary model paths without a config are still handled by
`tools/vad-asr-test` (it now understands the same formats).

**Limitations.** The file is decoded into memory entirely: ~115 MB RAM per hour of audio
(16 kHz mono int16) plus ~60 MB/hour for an hour of source 8 kHz — hour-long calls
are handled without problems, multi-hour audiobooks are harder. During transcription
the model is loaded as a second instance (the dictating pipeline is untouched) — RAM
is temporarily doubled. Speed is the same as dictation: zipformer-ru RTF ~0.07
on 2 cores, an hour-long call ≈ 4–5 minutes.

## Tools

| File                                                                                          | Purpose                                                                                                                                                                                                              |
| --------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tools/vad_asr_test.cpp`                                                                      | run an audio file (WAV/MP3/RAW — the same formats as the application) through the production path (config → VAD → ASR → punctuation), prints segments, text, and RTF; supports explicit model paths without a config |
| `tools/gen_bpe_vocab.py`                                                                      | creates a dictionary for hotwords in ordinary words from `bpe.model`                                                                                                                                                 |
| `./src/voice-assistant --check`                                                               | diagnostics of configuration, models, injector, dictionary, and hotkey                                                                                                                                               |
| `./src/voice-assistant --record N file.wav`                                                   | record from microphone to WAV without GUI (raw path, no AGC)                                                                                                                                                         |
| `voice-assistant --transcribe <file> [--profile <name>] [--out f.txt \| --notes \| --insert]` | transcribe an audio file (WAV/MP3/RAW/OGG/FLAC…) with any model from the config; text to stdout                                                                                                                      |
| `./src/voice-assistant --note "text"`                                                         | append a note to the file from the console (without microphone or X)                                                                                                                                                 |
| `./src/voice-assistant --pin-info`                                                            | output diagnostics: active window, WM_CLASS, PID, `pin_mode`, what `pin_window` would remember                                                                                                                       |
| `voice-assistant --download-model list\|<id>\|all [--force]`                                  | model catalog and download: sha256, resume, atomic unpacking                                                                                                                                                         |
| `./src/voice-assistant --type "text" [--pin-active] [--window WID] [--delay ms]`              | test insertion separately from recognition                                                                                                                                                                           |
| `./tools/vad-asr-test … --notes`                                                              | same, but segments from the file become notes: run a recording into the notes file                                                                                                                                   |

```bash
./tools/vad-asr-test recording.wav --config ~/.config/voice-assistant/settings.ini \
    [--profile gigaam-v3] [--hotwords words.txt] [--threads 1] [--no-punct] \
    [--notes] [--notes-file ~/work/notes.md] \
    [--rate 8000] [--channels 1] [--format s16le|s8u|f32le|alaw|ulaw]
```

## Tests

```bash
cd build && ctest --output-on-failure
```

* `CommandParserTest` — command parsing;
* `VoiceUnitsTest` — 66 checks: command dictionary and normalization, text
  post-processing (voice punctuation, capitalization, spaces), parsing `settings.ini`
  with profiles and paths, injector in dry-run mode, spelling against a real dictionary
  (skipped via `QSKIP` if hunspell or the dictionary is missing),
  WAV recording (header, data round-trip, validity for external tools),
  evdev hotkey logic on synthetic `input_event` — no hardware is needed for the tests.
  Separately covered: the notes file (`FileInjector`: day filename, header,
  timestamp, Cyrillic, “удали слово”, explicit file without markdown),
  target switching commands and their disabling, `TypingGuard` on synthetic
  time, the `keyActivity` signal and the `EVIOCGRAB` policy — including that
  the main keyboard is not grabbed.
  Separately covered is window selection for insertion (`output/WindowTarget.h`): where
  text will go when pinned to a window — activation + real events or `--window`
  with synthetic events, fallback to the active window if the pinned one was closed or
  is the assistant’s own window, focus return and `pin_mode` parsing. This is a pure
  function without X11, so it is tested without a display. The custom commands file is
  covered via `CommandDictionary::parseLine` (valid lines, comments, broken types)
  and a `commandToSpec` round-trip over all built-in commands — the same function
  validates lines in the “Commands” tab before saving. The model catalog
  (id uniqueness, release URLs, full sha256, correspondence to profiles) and
  the downloader’s `sha256File` are verified against a reference value from FIPS 180-2.

Besides unit tests, there is an e2e test bench for binding output to a window —
`tools/e2e/run-e2e.sh` (13 scenarios): it starts Xvfb + openbox, opens test Qt windows
with raw xcb event interception (you can see whether input arrived via real events or
via synthetic `XSendEvent`) and runs `--type` in all combinations of
`pin_mode`/`method`. It is not registered in ctest — X is required.
Details: `tools/e2e/README.md`.

## Architecture

```
microphone → QtAudioCapture → Agc → ApplicationController
                                      ↓
                                 VoicePipeline
                     SileroVad → segment → IRecognizer → TextPostProcessor
                                      ↓
                    CommandParser (command)  /  ITextInjector (text)
```

* `ApplicationController` is thin: modes, tray, D-Bus, result routing.
* Model selection is handled by `ConfigManager` + `RecognizerFactory`; the whole
  application knows only the `IRecognizer` interface.
* `SileroVad` returns **whole speech segments**: the non-streaming model receives
  the phrase as a whole, not 32 ms slices.

Description of layers, signal order, and object ownership —
in [docs/architecture.md](docs/architecture.md).

## Performance

Measurement on a Russian phrase of 7.16 s (2 threads, server CPU; on Celeron multiply by 3–5):

| ASR profile                | Size   | Decode     | RTF      | Russian                                |
| -------------------------- | ------ | ---------- | -------- | -------------------------------------- |
| **zipformer-ru** (default) | 110 MB | **397 ms** | **0.06** | accurate                               |
| GigaAM v3 RNN-T            | 167 MB | 1905 ms    | 0.27     | more accurate on noise                 |
| GigaAM v3 CTC              | 163 MB | 1256 ms    | 0.16     | accurate, without hotwords             |
| Whisper base               | 208 MB | 3663 ms    | 0.48     | worse, but has punctuation and English |

Full tables, methodology, and the conclusion why SenseVoice is not suitable for Russian —
in [docs/models.md](docs/models.md).

## Roadmap

* [x] `Mode::Spellcheck`: hunspell with a Russian dictionary (`AspellChecker` stub removed)
* [x] Settings dialog: five tabs (model with readiness status, hotkey with key
  
      capture, output, hotwords, commands); built-in commands are visible as “phrase → action”,
      custom ones are edited with validation; everything is applied without restart
* [ ] Auto-replace a found error with the first option by voice command
* [x] Push-to-talk: global hotkey via `evdev` (X11, Wayland, TTY)
* [x] Notes to file as a second output target — dictation does not interfere with typing
* [x] Audio file transcription: “разбери файл” / tray item / `--transcribe`
  
      (WAV, telephone RAW/G.711, MP3/OGG/FLAC/M4A via system GStreamer)
* [ ] Draft panel: dictated text accumulates in the assistant window and is inserted by command
* [ ] Wayland/TTY: `ITextInjector` implementation over `ydotool` (the hotkey already works there — evdev)
* [ ] Real-time partial results (streaming zipformer)
* [ ] Model-based punctuation, if a Russian one appears (`cxx::OfflinePunctuation` API already exists)

## Documentation

| File                                                                                   | About                                               |
| -------------------------------------------------------------------------------------- | --------------------------------------------------- |
| [docs/architecture.md](docs/architecture.md)                                           | layers, signal chain, object ownership              |
| [docs/configuration.md](docs/configuration.md)                                         | `settings.ini` reference, profiles, paths, commands |
| [docs/models.md](docs/models.md)                                                       | models, licenses, accuracy and speed measurements   |
| [docs/hotwords-and-punctuation.md](docs/hotwords-and-punctuation.md)                   | user hotwords and punctuation                       |
| [docs/sherpa-onnx-notes.md](docs/sherpa-onnx-notes.md)                                 | actual sherpa-onnx API, ABI, AUTOMOC                |
| [docs/integration-ApplicationController.md](docs/integration-ApplicationController.md) | how the controller is structured                    |
| [docs/troubleshooting.md](docs/troubleshooting.md)                                     | symptoms and treatment                              |
| [docs/development-log.md](docs/development-log.md)                                     | working notes by iterations                         |

## License

Project code — [MIT](LICENSE).

Third-party components are distributed under their own licenses and are **not included**
in the repository (downloaded separately):

| Component                                                                                                                            | License                    |
| ------------------------------------------------------------------------------------------------------------------------------------ | -------------------------- |
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx)                                                                                 | Apache-2.0                 |
| [Silero VAD](https://github.com/snakers4/silero-vad)                                                                                 | MIT                        |
| [zipformer-ru (icefall)](https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-transducer/zipformer-transducer-models.html) | Apache-2.0                 |
| [GigaAM v2/v3](https://github.com/salute-developers/GigaAM)                                                                          | MIT                        |
| [Whisper](https://github.com/openai/whisper)                                                                                         | MIT                        |
| Qt 5                                                                                                                                 | LGPL-3.0 (dynamic linking) |

## Acknowledgements

* [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) — inference and ready-made
  ONNX models, including GigaAM conversion.
* [Salute Developers (GigaAM)](https://github.com/salute-developers/GigaAM) — the best
  open Russian acoustic models.
* [Silero](https://github.com/snakers4/silero-vad) — VAD that runs on weak hardware.