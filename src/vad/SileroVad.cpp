#include "SileroVad.h"

#include <sherpa-onnx/c-api/cxx-api.h>

#include <QFile>
#include <QDebug>
#include <vector>

namespace {

// QByteArray (PCM16 mono little-endian) -> float [-1.0, 1.0]
std::vector<float> toFloatSamples(const QByteArray& data)
{
    const int n = static_cast<int>(data.size()) / static_cast<int>(sizeof(int16_t));
    std::vector<float> out;
    if (n <= 0) {
        return out;
    }
    out.resize(static_cast<size_t>(n));

    const int16_t* src = reinterpret_cast<const int16_t*>(data.constData());
    for (int i = 0; i < n; ++i) {
        out[static_cast<size_t>(i)] = static_cast<float>(src[i]) / 32768.0f;
    }
    return out;
}

// float [-1.0, 1.0] -> QByteArray (PCM16 mono little-endian)
QByteArray toPcm16(const std::vector<float>& samples)
{
    QByteArray out;
    if (samples.empty()) {
        return out;
    }
    out.resize(static_cast<int>(samples.size() * sizeof(int16_t)));
    int16_t* dst = reinterpret_cast<int16_t*>(out.data());

    for (size_t i = 0; i < samples.size(); ++i) {
        float v = samples[i] * 32768.0f;
        if (v > 32767.0f)  v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dst[i] = static_cast<int16_t>(v);
    }
    return out;
}

}  // namespace

SileroVad::SileroVad(const QString& modelPath, QObject* parent)
    : IVad(parent)
    , m_modelPath(modelPath)
{
}

SileroVad::SileroVad(const QString& modelPath, const Options& options, QObject* parent)
    : IVad(parent)
    , m_modelPath(modelPath)
    , m_options(options)
{
}

// Деструктор определён здесь, где sherpa_onnx::cxx::VoiceActivityDetector —
// полный тип (в заголовке он только объявлен).
SileroVad::~SileroVad() = default;

bool SileroVad::initialize()
{
    namespace cxx = sherpa_onnx::cxx;

    if (m_vad) {
        return true;  // уже инициализирован
    }

    if (m_modelPath.isEmpty() || !QFile::exists(m_modelPath)) {
        const QString msg = QStringLiteral("Silero VAD model not found: %1").arg(m_modelPath);
        qCritical().noquote() << msg;
        emit errorOccurred(msg);
        return false;
    }

    cxx::VadModelConfig config;
    config.silero_vad.model               = m_modelPath.toStdString();
    config.silero_vad.threshold           = m_options.threshold;
    config.silero_vad.min_silence_duration = m_options.minSilenceDuration;
    config.silero_vad.min_speech_duration  = m_options.minSpeechDuration;
    config.silero_vad.max_speech_duration  = m_options.maxSpeechDuration;
    config.silero_vad.window_size          = m_options.windowSize;

    config.sample_rate = kSampleRate;
    config.num_threads = m_options.numThreads;
    config.provider    = "cpu";
    config.debug       = m_options.debug;

    // Create() — статическая фабрика. Исключений sherpa-onnx C API не бросает:
    // при ошибке возвращается обёртка с нулевым хендлом, поэтому проверяем Get().
    auto vad = cxx::VoiceActivityDetector::Create(config, m_options.bufferSizeSeconds);
    if (vad.Get() == nullptr) {
        const QString msg = QStringLiteral(
            "VoiceActivityDetector::Create failed for %1 (см. вывод sherpa-onnx выше)")
                                .arg(m_modelPath);
        qCritical().noquote() << msg;
        emit errorOccurred(msg);
        return false;
    }

    m_vad = std::make_unique<cxx::VoiceActivityDetector>(std::move(vad));

    qDebug() << "SileroVad initialized:" << m_modelPath
             << "threshold=" << m_options.threshold
             << "minSilence=" << m_options.minSilenceDuration
             << "windowSize=" << m_options.windowSize;
    return true;
}

void SileroVad::processAudio(const QByteArray& audioData, int sampleRate)
{
    namespace cxx = sherpa_onnx::cxx;

    if (!m_vad) {
        qWarning() << "SileroVad::processAudio called before initialize()";
        return;
    }
    if (audioData.isEmpty()) {
        return;
    }
    if (sampleRate != kSampleRate) {
        // VAD в sherpa-onnx сам не ресемплит — только 16 кГц.
        emit errorOccurred(QStringLiteral("SileroVad: need %1 Hz mono int16, got %2 Hz")
                               .arg(kSampleRate)
                               .arg(sampleRate));
        return;
    }

    const std::vector<float> samples = toFloatSamples(audioData);
    if (samples.empty()) {
        return;
    }

    // Длину чанка выравнивать не нужно: sherpa-onnx буферизует вход сам
    // (VoiceActivityDetectorImpl::AcceptWaveform копит сэмплы и режет по window_size).
    m_vad->AcceptWaveform(samples.data(), static_cast<int32_t>(samples.size()));

    // 1. Фронт/тыл речи — для UI и для режима push-to-talk.
    const bool detected = m_vad->IsDetected();
    if (detected && !m_isSpeaking) {
        m_isSpeaking = true;
        emit speechStarted();
    } else if (!detected && m_isSpeaking) {
        m_isSpeaking = false;
        emit speechEnded();
    }

    // 2. Обязательно выгребаем готовые сегменты: если их не забирать,
    //    очередь внутри VAD растёт бесконечно.
    while (!m_vad->IsEmpty()) {
        cxx::SpeechSegment segment = m_vad->Front();
        m_vad->Pop();

        if (segment.samples.empty()) {
            continue;
        }
        const double durationSec =
            static_cast<double>(segment.samples.size()) / static_cast<double>(kSampleRate);
        if (durationSec < 0.1) {
            continue;  // щелчок/вздох
        }

        ++m_segmentsDetected;
        emit speechSegmentReady(toPcm16(segment.samples), kSampleRate);
    }
}

void SileroVad::flush()
{
    namespace cxx = sherpa_onnx::cxx;

    if (!m_vad) {
        return;
    }
    m_vad->Flush();

    while (!m_vad->IsEmpty()) {
        cxx::SpeechSegment segment = m_vad->Front();
        m_vad->Pop();
        if (segment.samples.empty()) {
            continue;
        }
        const double durationSec =
            static_cast<double>(segment.samples.size()) / static_cast<double>(kSampleRate);
        if (durationSec < 0.1) {
            continue;
        }
        ++m_segmentsDetected;
        emit speechSegmentReady(toPcm16(segment.samples), kSampleRate);
    }

    if (m_isSpeaking) {
        m_isSpeaking = false;
        emit speechEnded();
    }
}

void SileroVad::reset()
{
    if (m_vad) {
        m_vad->Clear();   // выбросить ненайденные сегменты
        m_vad->Reset();   // сбросить состояние модели
    }
    m_isSpeaking = false;
}
