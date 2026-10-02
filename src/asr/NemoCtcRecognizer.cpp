#include "NemoCtcRecognizer.h"

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

NemoCtcRecognizer::NemoCtcRecognizer(const QString& modelPath,
                                     const QString& tokensPath,
                                     int numThreads,
                                     QObject* parent)
    : IRecognizer(parent)
    , m_modelPath(modelPath)
    , m_tokensPath(tokensPath)
{
    m_options.numThreads = numThreads;
}

NemoCtcRecognizer::NemoCtcRecognizer(const QString& modelPath,
                                     const QString& tokensPath,
                                     const Options& options,
                                     QObject* parent)
    : IRecognizer(parent)
    , m_modelPath(modelPath)
    , m_tokensPath(tokensPath)
    , m_options(options)
{
}

NemoCtcRecognizer::~NemoCtcRecognizer() = default;

bool NemoCtcRecognizer::initialize()
{
    namespace cxx = sherpa_onnx::cxx;

    if (m_recognizer) {
        return true;
    }

    const auto check = [this](const QString& path, const char* what) {
        if (path.isEmpty() || !QFile::exists(path)) {
            const QString msg = QStringLiteral("NeMo CTC: %1 not found: %2").arg(what, path);
            qCritical().noquote() << msg;
            emit errorOccurred(msg);
            return false;
        }
        return true;
    };
    if (!check(m_modelPath, "model"))  return false;
    if (!check(m_tokensPath, "tokens")) return false;

    cxx::OfflineRecognizerConfig config;

    config.feat_config.sample_rate = kFallbackSampleRate;
    config.feat_config.feature_dim = 80;

    config.model_config.nemo_ctc.model = m_modelPath.toStdString();
    config.model_config.tokens         = m_tokensPath.toStdString();
    config.model_config.num_threads    = m_options.numThreads;
    config.model_config.provider       = "cpu";
    config.model_config.debug          = m_options.debug;

    // Для CTC в sherpa-onnx работает только greedy_search
    config.decoding_method = "greedy_search";

    auto recognizer = cxx::OfflineRecognizer::Create(config);
    if (recognizer.Get() == nullptr) {
        const QString msg = QStringLiteral("NeMo CTC: OfflineRecognizer::Create failed "
                                           "(см. вывод sherpa-onnx выше)");
        qCritical().noquote() << msg;
        emit errorOccurred(msg);
        return false;
    }

    m_recognizer = std::make_unique<cxx::OfflineRecognizer>(std::move(recognizer));
    m_loaded     = true;

    qDebug().noquote() << QString("NeMo CTC initialized: %1 threads=%2")
                              .arg(m_modelPath)
                              .arg(m_options.numThreads);
    return true;
}

void NemoCtcRecognizer::acceptWaveform(const QByteArray& audioData, int sampleRate)
{
    if (!m_recognizer) {
        qWarning() << "NemoCtcRecognizer::acceptWaveform called before initialize()";
        return;
    }
    if (audioData.isEmpty() || sampleRate <= 0) {
        return;
    }

    if (m_buffer.empty()) {
        m_bufferSampleRate = sampleRate;
    } else if (sampleRate != m_bufferSampleRate) {
        qWarning() << "NemoCtcRecognizer: sample rate changed"
                   << m_bufferSampleRate << "->" << sampleRate << ", buffer dropped";
        m_buffer.clear();
        m_bufferSampleRate = sampleRate;
    }

    appendPcm16(m_buffer, audioData);
}

QString NemoCtcRecognizer::finalResult()
{
    return decodeBuffer();
}

QString NemoCtcRecognizer::recognize(const QByteArray& pcm16, int sampleRate)
{
    m_buffer.clear();
    m_bufferSampleRate = (sampleRate > 0) ? sampleRate : kFallbackSampleRate;
    appendPcm16(m_buffer, pcm16);
    return decodeBuffer();
}

QString NemoCtcRecognizer::decodeBuffer()
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
        emit errorOccurred(QStringLiteral("NeMo CTC: CreateStream() failed"));
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
    qDebug().noquote() << QString("ASR[nemo-ctc]: %1 с -> \"%2\"")
                              .arg(seconds, 0, 'f', 2)
                              .arg(text);
    return text;
}

void NemoCtcRecognizer::reset()
{
    m_buffer.clear();
    m_bufferSampleRate = kFallbackSampleRate;
}

void NemoCtcRecognizer::setHotwords(const QStringList& words, float score)
{
    Q_UNUSED(score)
    // CTC-модели hotwords не поддерживают вообще: контекстный граф в sherpa-onnx
    // строится только в transducer-имплементациях.
    if (!words.isEmpty() && !m_hotwordsWarned) {
        m_hotwordsWarned = true;
        qWarning().noquote()
            << QString("NeMo CTC игнорирует hotwords (%1 шт.): CTC их не поддерживает. "
                       "Нужны подсказки — бери профиль с engine=transducer.")
                   .arg(words.size());
    }
}

bool NemoCtcRecognizer::isLoaded() const
{
    return m_loaded;
}
