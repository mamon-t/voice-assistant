# Интеграция VoicePipeline в ApplicationController

Патч к текущему `src/core/ApplicationController.{h,cpp}` (тот, что с `m_agc`,
`m_audioBuffer` и `TARGET_CHUNK_SIZE = 1600`). Контроллер остаётся тонким: вся
речевая цепочка живёт в `VoicePipeline`.

---

## 1. `ApplicationController.h`

```cpp
#pragma once

#include <QObject>
#include <QByteArray>
#include <memory>
#include "core/Mode.h"

class IAudioCapture;
class Agc;
class ConfigManager;      // +++
class VoicePipeline;      // +++
class ITextInjector;      // +++ (если уже инжектируешь текст)
class CommandParser;      // +++

class ApplicationController : public QObject {
    Q_OBJECT

public:
    explicit ApplicationController(QObject* parent = nullptr);
    ~ApplicationController();

    Mode mode() const;
    void startRecording();
    void stopRecording();
    void setMode(Mode mode);

    // +++ смена ASR-профиля на лету (переключатель "шумно/быстро" в трее)
    bool switchAsrProfile(const QString& profileName);
    QString activeAsrProfile() const;

signals:
    void modeChanged(Mode mode);
    void errorOccurred(const QString& message);
    void textRecognized(const QString& text);   // +++ для UI/лога

private slots:
    void onAudioDataReady(const QByteArray& data, int sampleRate);
    void onAudioError(const QString& message);
    void onTextReady(const QString& text);      // +++

private:
    bool buildPipeline();                       // +++

    Mode m_mode = Mode::Off;
    std::unique_ptr<IAudioCapture> m_audioCapture;
    std::unique_ptr<Agc> m_agc;
    std::unique_ptr<ConfigManager>  m_config;    // +++
    std::unique_ptr<VoicePipeline>  m_pipeline;  // +++
    std::unique_ptr<ITextInjector>  m_injector;  // +++ (XdotoolInjector/YdotoolInjector)
    std::unique_ptr<CommandParser>  m_commands;  // +++

    QByteArray m_audioBuffer;
    static constexpr int TARGET_CHUNK_SIZE = 1600; // 50 мс при 16 кГц
};
```

## 2. `ApplicationController.cpp` — конструктор

```cpp
#include "ApplicationController.h"
#include "audio/QtAudioCapture.h"
#include "audio/Agc.h"
#include "config/ConfigManager.h"        // +++
#include "core/VoicePipeline.h"          // +++
#include "commands/CommandParser.h"      // +++
#include "output/XdotoolInjector.h"      // +++
#include <QDebug>

ApplicationController::ApplicationController(QObject* parent)
    : QObject(parent)
{
    // 1. Аудиоподсистема — как было
    m_audioCapture = std::make_unique<QtAudioCapture>();
    if (!m_audioCapture->initialize()) {
        emit errorOccurred(QStringLiteral("Failed to initialize audio capture"));
        return;
    }

    // 2. AGC — как был
    m_agc = std::make_unique<Agc>();
    m_agc->setTargetDb(-15.0f);
    m_agc->setAttackTime(0.05f);
    m_agc->setReleaseTime(0.2f);
    m_agc->setMaxGain(10.0f);
    m_agc->setMinGain(0.01f);

    // 3. +++ Конфиг и речевой пайплайн (VAD -> ASR -> пунктуация)
    m_config = std::make_unique<ConfigManager>();
    if (!buildPipeline()) {
        return;   // ошибка уже отправлена сигналом из buildPipeline()
    }

    // 4. +++ Инжектор текста и парсер команд
    m_injector = std::make_unique<XdotoolInjector>();
    if (!m_injector->initialize() || !m_injector->isAvailable()) {
        emit errorOccurred(QStringLiteral("Text injector недоступен (xdotool/ydotool?)"));
    }
    m_commands = std::make_unique<CommandParser>();

    // 5. Сигналы аудио — как было
    connect(m_audioCapture.get(), &IAudioCapture::audioDataReady,
            this, &ApplicationController::onAudioDataReady);
    connect(m_audioCapture.get(), &IAudioCapture::errorOccurred,
            this, &ApplicationController::onAudioError);
}
```

## 3. `buildPipeline()` — сборка цепочки из конфига

```cpp
bool ApplicationController::buildPipeline()
{
    m_pipeline = std::make_unique<VoicePipeline>(
        VoicePipeline::loadSettings(*m_config), this);

    connect(m_pipeline.get(), &VoicePipeline::errorOccurred,
            this, &ApplicationController::errorOccurred);
    connect(m_pipeline.get(), &VoicePipeline::textReady,
            this, &ApplicationController::onTextReady);

    QString err;
    if (!m_pipeline->initialize(&err)) {
        emit errorOccurred(QStringLiteral("VoicePipeline: %1").arg(err));
        m_pipeline.reset();
        return false;
    }

    // Подсказки пользователя (HotwordsManager / user_hotwords.txt).
    // Диктантные знаки ("точка", "запятая", ...) VoicePipeline примешает сам.
    const QStringList hw = m_config->loadHotwords(m_config->userHotwordsPath());
    if (!hw.isEmpty()) {
        m_pipeline->setHotwords(hw, m_config->hotwordsScore());
    }

    qInfo().noquote() << QStringLiteral("ASR-профиль: %1")
                             .arg(m_pipeline->asrProfileName());
    return true;
}
```

## 4. `onAudioDataReady()` — AGC остался, дальше в пайплайн

```cpp
void ApplicationController::onAudioDataReady(const QByteArray& data, int sampleRate)
{
    m_audioBuffer.append(data);

    while (m_audioBuffer.size() >= TARGET_CHUNK_SIZE) {
        const QByteArray chunk = m_audioBuffer.left(TARGET_CHUNK_SIZE);
        m_audioBuffer.remove(0, TARGET_CHUNK_SIZE);

        const QByteArray processed = m_agc->process(chunk, sampleRate);

        // было: RMS в qDebug. стало: отдаём в речевой пайплайн
        if (m_mode != Mode::Off && m_pipeline) {
            m_pipeline->processAudio(processed, sampleRate);
        }
    }
}
```

ВАЖНО: `QtAudioCapture` должен отдавать **16 кГц моно int16**, иначе Silero VAD
откажется работать (он не ресемплит). Если захват идёт в 44.1/48 кГц — ресемплить
надо до `processAudio()` (например, `QAudioFormat` с `setSampleRate(16000)`,
либо `LinearResampler` из sherpa-onnx).

## 5. `onTextReady()` — маршрутизация по режиму

```cpp
void ApplicationController::onTextReady(const QString& text)
{
    emit textRecognized(text);

    switch (m_mode) {
    case Mode::Dictation:
        if (m_injector && m_injector->isAvailable()) {
            m_injector->typeText(text);
        }
        break;

    case Mode::Edit:
    case Mode::Spellcheck:
        if (m_commands) {
            if (const auto cmd = m_commands->parse(text); cmd.has_value()) {
                // выполнить команду (DeleteWord / NewLine / SetMode / Punctuation ...)
            } else if (m_mode == Mode::Dictation) {
                m_injector->typeText(text);
            }
        }
        break;

    case Mode::Off:
    case Mode::Error:
        break;
    }
}
```

## 6. Push-to-talk

```cpp
// отпускание клавиши PTT:
m_pipeline->flush();     // вытолкнуть хвост фразы -> придёт textReady

// начало новой записи:
m_agc->reset();
m_audioBuffer.clear();
m_pipeline->reset();     // сбросить состояние VAD и буфер ASR
```

## 7. Смена профиля на лету

```cpp
bool ApplicationController::switchAsrProfile(const QString& profileName)
{
    if (!m_config->hasAsrProfile(profileName)) {
        emit errorOccurred(QStringLiteral("Нет профиля ASR: %1").arg(profileName));
        return false;
    }
    m_config->setActiveAsrProfileName(profileName);

    const Mode old = m_mode;
    if (m_pipeline) {
        disconnect(m_pipeline.get(), nullptr, this, nullptr);
        m_pipeline.reset();               // модель выгружается, ОЗУ освобождается
    }
    if (!buildPipeline()) {
        m_mode = Mode::Error;
        emit modeChanged(m_mode);
        return false;
    }
    setMode(old);
    return true;
}

QString ApplicationController::activeAsrProfile() const
{
    return m_pipeline ? m_pipeline->asrProfileName() : QString();
}
```

Переинициализация стоит 1.2–5.1 с (замерено: GigaAM CTC 1.7 с, GigaAM RNN-T 2.9 с,
zipformer-ru 5.1 с, Whisper 1.2 с) — делать только по явному действию пользователя,
не на каждый чанк.
