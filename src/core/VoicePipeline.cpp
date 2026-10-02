#include "VoicePipeline.h"

#include "asr/IRecognizer.h"
#include "asr/RecognizerFactory.h"
#include "config/ConfigManager.h"
#include "vad/SileroVad.h"

#include <QDebug>

VoicePipeline::VoicePipeline(const Settings& settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_post(settings.text)
    , m_hotwordsScore(settings.asr.hotwordsScore)
{
}

VoicePipeline::~VoicePipeline() = default;

VoicePipeline::Settings VoicePipeline::loadSettings(const ConfigManager& config)
{
    Settings s;

    s.vadModelPath     = config.sileroVadPath();
    s.vadThreshold     = config.vadThreshold();
    s.vadMinSilence    = config.vadMinSilenceDuration();
    s.vadMinSpeech     = config.vadMinSpeechDuration();
    s.vadWindowSize    = config.vadWindowSize();
    s.vadNumThreads    = config.vadNumThreads();
    s.vadBufferSeconds = config.vadBufferSeconds();

    s.asr = config.activeAsrProfile();

    s.text.voicePunctuation    = config.voicePunctuation();
    s.text.capitalizeSentences = config.autoPunctuate();
    s.text.addFinalDot         = config.autoPunctuate();
    s.text.collapseSpaces      = true;

    return s;
}

bool VoicePipeline::initialize(QString* error)
{
    const auto fail = [this, error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        qCritical().noquote() << "VoicePipeline:" << msg;
        emit errorOccurred(msg);
        return false;
    };

    if (m_settings.vadModelPath.isEmpty()) {
        return fail(QStringLiteral("не задан путь к silero_vad.onnx ([vad] model в settings.ini)"));
    }

    // --- VAD ---
    SileroVad::Options vadOpt;
    vadOpt.threshold          = m_settings.vadThreshold;
    vadOpt.minSilenceDuration = m_settings.vadMinSilence;
    vadOpt.minSpeechDuration  = m_settings.vadMinSpeech;
    vadOpt.maxSpeechDuration  = m_settings.vadMaxSpeech;
    vadOpt.windowSize         = m_settings.vadWindowSize;
    vadOpt.numThreads         = m_settings.vadNumThreads;
    vadOpt.bufferSizeSeconds  = m_settings.vadBufferSeconds;

    m_vad = std::make_unique<SileroVad>(m_settings.vadModelPath, vadOpt, this);

    connect(m_vad.get(), &IVad::speechStarted, this, &VoicePipeline::speechStarted);
    connect(m_vad.get(), &IVad::speechEnded,   this, &VoicePipeline::speechEnded);
    connect(m_vad.get(), &IVad::errorOccurred, this, &VoicePipeline::errorOccurred);
    connect(m_vad.get(), &SileroVad::speechSegmentReady,
            this, &VoicePipeline::onSpeechSegment);

    if (!m_vad->initialize()) {
        return fail(QStringLiteral("SileroVad::initialize() вернул false для %1")
                        .arg(m_settings.vadModelPath));
    }

    // --- ASR ---
    QString factoryError;
    m_asr = RecognizerFactory::create(m_settings.asr, this, &factoryError);
    if (!m_asr) {
        return fail(factoryError.isEmpty()
                        ? QStringLiteral("не удалось создать распознаватель для профиля '%1'")
                              .arg(m_settings.asr.name)
                        : factoryError);
    }

    connect(m_asr.get(), &IRecognizer::errorOccurred, this, &VoicePipeline::errorOccurred);

    if (!m_asr->initialize()) {
        return fail(QStringLiteral("ASR '%1' (engine=%2) не инициализировался")
                        .arg(m_settings.asr.name,
                             AsrProfile::engineToString(m_settings.asr.engine)));
    }

    m_ready = true;

    // Диктантные знаки сразу в hotwords: иначе ASR их проглатывает и пунктуация не появится
    applyHotwords();

    qInfo().noquote() << QString("VoicePipeline готов: VAD=%1 | ASR=%2 (%3) | потоков=%4 | пунктуация=%5")
                             .arg(m_settings.vadModelPath,
                                  m_settings.asr.name,
                                  AsrProfile::engineToString(m_settings.asr.engine))
                             .arg(m_settings.asr.numThreads)
                             .arg(m_settings.text.voicePunctuation ? QStringLiteral("голосовая")
                                                                    : QStringLiteral("выкл"));
    return true;
}

void VoicePipeline::processAudio(const QByteArray& pcm16Mono, int sampleRate)
{
    if (!m_ready || !m_vad) {
        return;
    }
    m_vad->processAudio(pcm16Mono, sampleRate);
}

void VoicePipeline::flush()
{
    if (m_vad) {
        m_vad->flush();
    }
}

void VoicePipeline::reset()
{
    if (m_vad) {
        m_vad->reset();
    }
    if (m_asr) {
        m_asr->reset();
    }
}

void VoicePipeline::setHotwords(const QStringList& words, float score)
{
    m_userHotwords  = words;
    m_hotwordsScore = score;
    applyHotwords();
}

void VoicePipeline::applyHotwords()
{
    if (!m_asr) {
        return;   // ещё не инициализированы — применим в initialize()
    }

    QStringList effective = m_userHotwords;

    // Диктантные знаки ("точка", "запятая", ...) добавляем сами: замер показал,
    // что без буста модель их просто не выдаёт, и голосовая пунктуация не работает.
    // Смысл есть только для transducer в modified_beam_search — остальные движки
    // hotwords игнорируют.
    const bool canBoost = m_settings.text.voicePunctuation
        && m_settings.asr.engine == AsrProfile::Engine::Transducer
        && m_settings.asr.decodingMethod == QLatin1String("modified_beam_search");

    if (canBoost) {
        const QStringList punct = m_post.punctuationPhrases();
        for (const QString& p : punct) {
            if (!effective.contains(p)) {
                effective << p;
            }
        }
    }

    m_asr->setHotwords(effective, m_hotwordsScore);
}

void VoicePipeline::onSpeechSegment(const QByteArray& pcm16, int sampleRate)
{
    if (!m_asr) {
        return;
    }

    m_asr->acceptWaveform(pcm16, sampleRate);
    const QString raw = m_asr->finalResult();
    if (raw.isEmpty()) {
        return;
    }

    emit rawTextRecognized(raw);

    const QString text = m_post.process(raw);
    if (!text.isEmpty()) {
        emit textReady(text);
    }
}
