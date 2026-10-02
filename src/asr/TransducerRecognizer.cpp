#include "TransducerRecognizer.h"

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

TransducerRecognizer::TransducerRecognizer(const QString& encoderPath,
                                         const QString& decoderPath,
                                         const QString& joinerPath,
                                         const QString& tokensPath,
                                         int numThreads,
                                         QObject* parent)
    : IRecognizer(parent)
    , m_encoderPath(encoderPath)
    , m_decoderPath(decoderPath)
    , m_joinerPath(joinerPath)
    , m_tokensPath(tokensPath)
{
    m_options.numThreads = numThreads;
    m_hotwordsScore      = m_options.hotwordsScore;
}

TransducerRecognizer::TransducerRecognizer(const QString& encoderPath,
                                         const QString& decoderPath,
                                         const QString& joinerPath,
                                         const QString& tokensPath,
                                         const Options& options,
                                         QObject* parent)
    : IRecognizer(parent)
    , m_encoderPath(encoderPath)
    , m_decoderPath(decoderPath)
    , m_joinerPath(joinerPath)
    , m_tokensPath(tokensPath)
    , m_options(options)
{
    m_hotwordsScore = m_options.hotwordsScore;
}

TransducerRecognizer::~TransducerRecognizer() = default;

bool TransducerRecognizer::setOptionPath(const QString& path, const char* what)
{
    if (path.isEmpty() || !QFile::exists(path)) {
        const QString msg = QStringLiteral("Transducer: %1 not found: %2").arg(what, path);
        qCritical().noquote() << msg;
        emit errorOccurred(msg);
        return false;
    }
    return true;
}

bool TransducerRecognizer::initialize()
{
    namespace cxx = sherpa_onnx::cxx;

    if (m_recognizer) {
        return true;
    }

    if (!setOptionPath(m_encoderPath, "encoder")) return false;
    if (!setOptionPath(m_decoderPath, "decoder")) return false;
    if (!setOptionPath(m_joinerPath,  "joiner"))  return false;
    if (!setOptionPath(m_tokensPath,  "tokens"))  return false;

    if (!m_options.bpeVocab.isEmpty() && !QFile::exists(m_options.bpeVocab)) {
        qWarning().noquote()
            << QString("Transducer: bpeVocab '%1' не найден — hotwords словами работать не будут")
                   .arg(m_options.bpeVocab);
    }

    cxx::OfflineRecognizerConfig config;

    config.feat_config.sample_rate = kFallbackSampleRate;
    config.feat_config.feature_dim = 80;

    // Offline transducer: encoder + decoder + joiner
    config.model_config.transducer.encoder  = m_encoderPath.toStdString();
    config.model_config.transducer.decoder  = m_decoderPath.toStdString();
    config.model_config.transducer.joiner   = m_joinerPath.toStdString();

    config.model_config.tokens      = m_tokensPath.toStdString();
    config.model_config.num_threads = m_options.numThreads;
    config.model_config.provider    = "cpu";
    config.model_config.debug       = m_options.debug;

    // BPE: нужно только если hotwords передаются обычными словами
    if (!m_options.modelingUnit.isEmpty()) {
        config.model_config.modeling_unit = m_options.modelingUnit.toStdString();
    }
    if (!m_options.bpeVocab.isEmpty()) {
        config.model_config.bpe_vocab = m_options.bpeVocab.toStdString();
        if (m_options.modelingUnit.isEmpty()) {
            config.model_config.modeling_unit = "bpe";
        }
    }

    config.decoding_method  = m_options.decodingMethod.toStdString();
    config.max_active_paths = m_options.maxActivePaths;
    config.hotwords_score   = m_options.hotwordsScore;

    auto recognizer = cxx::OfflineRecognizer::Create(config);
    if (recognizer.Get() == nullptr) {
        const QString msg = QStringLiteral("Transducer: OfflineRecognizer::Create failed "
                                           "(см. вывод sherpa-onnx выше)");
        qCritical().noquote() << msg;
        emit errorOccurred(msg);
        return false;
    }

    m_recognizer = std::make_unique<cxx::OfflineRecognizer>(std::move(recognizer));
    m_loaded     = true;

    qDebug().noquote()
        << QString("Transducer initialized: %1 | decoding=%2 threads=%3 hotwordsScore=%4 bpe=%5")
               .arg(m_encoderPath, m_options.decodingMethod)
               .arg(m_options.numThreads)
               .arg(m_options.hotwordsScore)
               .arg(m_options.bpeVocab.isEmpty() ? QStringLiteral("нет") : m_options.bpeVocab);
    return true;
}

void TransducerRecognizer::acceptWaveform(const QByteArray& audioData, int sampleRate)
{
    if (!m_recognizer) {
        qWarning() << "TransducerRecognizer::acceptWaveform called before initialize()";
        return;
    }
    if (audioData.isEmpty() || sampleRate <= 0) {
        return;
    }

    if (m_buffer.empty()) {
        m_bufferSampleRate = sampleRate;
    } else if (sampleRate != m_bufferSampleRate) {
        qWarning() << "TransducerRecognizer: sample rate changed"
                   << m_bufferSampleRate << "->" << sampleRate << ", buffer dropped";
        m_buffer.clear();
        m_bufferSampleRate = sampleRate;
    }

    // Ресемплинг не нужен: OfflineStream::AcceptWaveform() делает его сам.
    appendPcm16(m_buffer, audioData);
}

QString TransducerRecognizer::finalResult()
{
    return decodeBuffer();
}

QString TransducerRecognizer::recognize(const QByteArray& pcm16, int sampleRate)
{
    m_buffer.clear();
    m_bufferSampleRate = (sampleRate > 0) ? sampleRate : kFallbackSampleRate;
    appendPcm16(m_buffer, pcm16);
    return decodeBuffer();
}

QString TransducerRecognizer::decodeBuffer()
{
    namespace cxx = sherpa_onnx::cxx;

    if (!m_recognizer) {
        return QString();
    }
    if (m_buffer.size() < kMinUtteranceSamples) {
        m_buffer.clear();
        return QString();
    }

    // Hotwords применяются только в modified_beam_search: только там sherpa-onnx
    // строит контекстный граф (offline-recognizer-transducer-impl.h).
    const bool beam = (m_options.decodingMethod == QLatin1String("modified_beam_search"));
    const QString hotwords = m_hotwords.join(QLatin1Char('\n'));

    cxx::OfflineStream stream = (beam && !hotwords.isEmpty())
        ? m_recognizer->CreateStream(hotwords.toStdString())
        : m_recognizer->CreateStream();

    if (stream.Get() == nullptr) {
        emit errorOccurred(QStringLiteral("Transducer: CreateStream() failed"));
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
    qDebug().noquote() << QString("ASR[zipformer]: %1 с -> \"%2\"%3")
                              .arg(seconds, 0, 'f', 2)
                              .arg(text,
                                   (beam && !hotwords.isEmpty())
                                       ? QStringLiteral(" (hotwords: %1)").arg(m_hotwords.size())
                                       : QString());
    return text;
}

void TransducerRecognizer::reset()
{
    m_buffer.clear();
    m_bufferSampleRate = kFallbackSampleRate;
}

void TransducerRecognizer::setHotwords(const QStringList& words, float score)
{
    // В отличие от SenseVoice/Whisper, здесь hotwords работают по-настоящему:
    // это transducer, sherpa-onnx строит контекстный граф и бустит указанные
    // последовательности токенов (проверено: без подсказок модель теряла начало
    // фразы, с подсказками текст совпал с эталоном полностью).
    QStringList cleaned;
    cleaned.reserve(words.size());
    for (const QString& w : words) {
        const QString t = w.trimmed();
        if (!t.isEmpty()) {
            cleaned << t;
        }
    }

    m_hotwords      = cleaned;
    m_hotwordsScore = score;
    m_options.hotwordsScore = score;

    if (m_recognizer && score > 0.0f) {
        // hotwords_score живёт в конфиге распознавателя — обновляем его на лету
        namespace cxx = sherpa_onnx::cxx;
        cxx::OfflineRecognizerConfig config;
        config.feat_config.sample_rate            = kFallbackSampleRate;
        config.feat_config.feature_dim            = 80;
        config.model_config.transducer.encoder    = m_encoderPath.toStdString();
        config.model_config.transducer.decoder    = m_decoderPath.toStdString();
        config.model_config.transducer.joiner     = m_joinerPath.toStdString();
        config.model_config.tokens                = m_tokensPath.toStdString();
        config.model_config.num_threads           = m_options.numThreads;
        config.model_config.provider              = "cpu";
        config.model_config.debug                 = m_options.debug;
        if (!m_options.modelingUnit.isEmpty()) {
            config.model_config.modeling_unit = m_options.modelingUnit.toStdString();
        }
        if (!m_options.bpeVocab.isEmpty()) {
            config.model_config.bpe_vocab = m_options.bpeVocab.toStdString();
        }
        config.decoding_method  = m_options.decodingMethod.toStdString();
        config.max_active_paths = m_options.maxActivePaths;
        config.hotwords_score   = score;
        m_recognizer->SetConfig(config);
    }

    if (m_options.decodingMethod != QLatin1String("modified_beam_search") && !cleaned.isEmpty()) {
        qWarning().noquote()
            << QString("Transducer: hotwords (%1 шт.) применятся только при "
                       "decodingMethod=\"modified_beam_search\", сейчас \"%2\"")
                   .arg(cleaned.size())
                   .arg(m_options.decodingMethod);
    }

    qDebug().noquote() << QString("Transducer hotwords: %1 шт., score=%2")
                              .arg(cleaned.size())
                              .arg(score);
}

bool TransducerRecognizer::isLoaded() const
{
    return m_loaded;
}
