#include "WhisperRecognizer.h"

#include <sherpa-onnx/c-api/cxx-api.h>

#include <QFile>
#include <QDebug>

namespace {

constexpr int    kFallbackSampleRate  = 16000;
constexpr size_t kMinUtteranceSamples = 1600;  // 0.1 с @ 16 кГц

void appendPcm16(std::vector<float>& dst, const QByteArray& pcm16)
{
    const int n = static_cast<int>(pcm16.size()) / static_cast<int>(sizeof(int16_t));
    if (n <= 0) {
        return;
    }
    const int16_t* src = reinterpret_cast<const int16_t*>(pcm16.constData());
    dst.reserve(dst.size() + static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        dst.push_back(static_cast<float>(src[i]) / 32768.0f);
    }
}

}  // namespace

WhisperRecognizer::WhisperRecognizer(const QString& encoderPath,
                                     const QString& decoderPath,
                                     const QString& tokensPath,
                                     int numThreads,
                                     QObject* parent)
    : IRecognizer(parent)
    , m_encoderPath(encoderPath)
    , m_decoderPath(decoderPath)
    , m_tokensPath(tokensPath)
{
    m_options.numThreads = numThreads;
}

WhisperRecognizer::WhisperRecognizer(const QString& encoderPath,
                                     const QString& decoderPath,
                                     const QString& tokensPath,
                                     const Options& options,
                                     QObject* parent)
    : IRecognizer(parent)
    , m_encoderPath(encoderPath)
    , m_decoderPath(decoderPath)
    , m_tokensPath(tokensPath)
    , m_options(options)
{
}

WhisperRecognizer::~WhisperRecognizer() = default;

bool WhisperRecognizer::initialize()
{
    namespace cxx = sherpa_onnx::cxx;

    if (m_recognizer) {
        return true;
    }

    const auto check = [this](const QString& path, const char* what) {
        if (path.isEmpty() || !QFile::exists(path)) {
            const QString msg = QStringLiteral("Whisper %1 not found: %2").arg(what, path);
            qCritical().noquote() << msg;
            emit errorOccurred(msg);
            return false;
        }
        return true;
    };
    if (!check(m_encoderPath, "encoder")) return false;
    if (!check(m_decoderPath, "decoder")) return false;
    if (!check(m_tokensPath, "tokens"))   return false;

    cxx::OfflineRecognizerConfig config;

    config.feat_config.sample_rate = kFallbackSampleRate;
    config.feat_config.feature_dim = 80;

    config.model_config.whisper.encoder     = m_encoderPath.toStdString();
    config.model_config.whisper.decoder     = m_decoderPath.toStdString();
    config.model_config.whisper.language    = m_options.language.toStdString();
    config.model_config.whisper.task        = m_options.task.toStdString();
    config.model_config.whisper.tail_paddings = m_options.tailPaddings;

    config.model_config.tokens      = m_tokensPath.toStdString();
    config.model_config.num_threads = m_options.numThreads;
    config.model_config.provider    = "cpu";
    config.model_config.debug       = m_options.debug;

    config.decoding_method = "greedy_search";

    auto recognizer = cxx::OfflineRecognizer::Create(config);
    if (recognizer.Get() == nullptr) {
        const QString msg = QStringLiteral("Whisper: OfflineRecognizer::Create failed "
                                           "(см. вывод sherpa-onnx выше)");
        qCritical().noquote() << msg;
        emit errorOccurred(msg);
        return false;
    }

    m_recognizer = std::make_unique<cxx::OfflineRecognizer>(std::move(recognizer));
    m_loaded     = true;

    qDebug().noquote() << QString("Whisper initialized: enc=%1 dec=%2 lang=%3 threads=%4")
                              .arg(m_encoderPath, m_decoderPath, m_options.language)
                              .arg(m_options.numThreads);
    return true;
}

void WhisperRecognizer::acceptWaveform(const QByteArray& audioData, int sampleRate)
{
    if (!m_recognizer) {
        qWarning() << "WhisperRecognizer::acceptWaveform called before initialize()";
        return;
    }
    if (audioData.isEmpty() || sampleRate <= 0) {
        return;
    }

    if (m_buffer.empty()) {
        m_bufferSampleRate = sampleRate;
    } else if (sampleRate != m_bufferSampleRate) {
        qWarning() << "WhisperRecognizer: sample rate changed"
                   << m_bufferSampleRate << "->" << sampleRate << ", buffer dropped";
        m_buffer.clear();
        m_bufferSampleRate = sampleRate;
    }

    // Ресемплинг не нужен: OfflineStream::AcceptWaveform() делает его сам.
    appendPcm16(m_buffer, audioData);
}

QString WhisperRecognizer::finalResult()
{
    return decodeBuffer();
}

QString WhisperRecognizer::recognize(const QByteArray& pcm16, int sampleRate)
{
    m_buffer.clear();
    m_bufferSampleRate = (sampleRate > 0) ? sampleRate : kFallbackSampleRate;
    appendPcm16(m_buffer, pcm16);
    return decodeBuffer();
}

QString WhisperRecognizer::decodeBuffer()
{
    namespace cxx = sherpa_onnx::cxx;

    if (!m_recognizer) {
        return QString();
    }
    if (m_buffer.size() < kMinUtteranceSamples) {
        m_buffer.clear();
        return QString();
    }

    cxx::OfflineStream stream = m_recognizer->CreateStream();
    if (stream.Get() == nullptr) {
        emit errorOccurred(QStringLiteral("Whisper: CreateStream() failed"));
        m_buffer.clear();
        return QString();
    }

    stream.AcceptWaveform(m_bufferSampleRate,
                          m_buffer.data(),
                          static_cast<int32_t>(m_buffer.size()));

    m_recognizer->Decode(&stream);
    const cxx::OfflineRecognizerResult result = m_recognizer->GetResult(&stream);

    const double seconds = m_buffer.size() / static_cast<double>(m_bufferSampleRate);
    m_buffer.clear();

    const QString text = QString::fromStdString(result.text).trimmed();
    qDebug().noquote() << QString("ASR[whisper]: %1 с -> \"%2\" (lang=%3)")
                              .arg(seconds, 0, 'f', 2)
                              .arg(text, QString::fromStdString(result.lang));
    return text;
}

void WhisperRecognizer::reset()
{
    m_buffer.clear();
    m_bufferSampleRate = kFallbackSampleRate;
}

void WhisperRecognizer::setHotwords(const QStringList& words, float score)
{
    Q_UNUSED(score)
    // hotwords в sherpa-onnx поддерживают только transducer-модели
    // (offline-recognizer-transducer-impl.h), FunASR-Nano и Qwen3-ASR.
    // Для Whisper — только посткоррекция текста (HotwordsManager).
    if (!words.isEmpty() && !m_hotwordsWarned) {
        m_hotwordsWarned = true;
        qWarning().noquote()
            << QString("Whisper игнорирует hotwords (поддержка только у transducer-моделей). "
                       "Принято слов: %1 — используй посткоррекцию в HotwordsManager.")
                   .arg(words.size());
    }
}

bool WhisperRecognizer::isLoaded() const
{
    return m_loaded;
}
