#include "SenseVoiceRecognizer.h"
#include <sherpa-onnx/c-api/cxx-api.h>
#include <QDebug>

using namespace sherpa_onnx::cxx;

class SenseVoiceRecognizer::Impl {
public:
    std::unique_ptr<OfflineRecognizer> recognizer;
};

SenseVoiceRecognizer::SenseVoiceRecognizer(const QString& modelPath,
                                           const QString& tokensPath,
                                           int numThreads,
                                           QObject* parent)
    : IRecognizer(parent)
    , m_modelPath(modelPath)
    , m_tokensPath(tokensPath)
    , m_numThreads(numThreads)
    , m_impl(std::make_unique<Impl>())
{
}

SenseVoiceRecognizer::~SenseVoiceRecognizer() = default;

bool SenseVoiceRecognizer::initialize() {
    try {
        OfflineRecognizerConfig config;
        config.model_config.sense_voice.model = m_modelPath.toStdString();
        config.model_config.tokens = m_tokensPath.toStdString();
        config.model_config.num_threads = m_numThreads;
        config.model_config.debug = false;
        config.decoding_method = "greedy_search";
        
        m_impl->recognizer = std::make_unique<OfflineRecognizer>(config);
        
        qDebug() << "SenseVoice initialized successfully";
        m_loaded = true;
        return true;
    } catch (const std::exception& e) {
        qCritical() << "Failed to initialize SenseVoice:" << e.what();
        emit errorOccurred(QString("SenseVoice init error: %1").arg(e.what()));
        return false;
    }
}

void SenseVoiceRecognizer::acceptWaveform(const QByteArray& audioData, int sampleRate) {
    if (!m_impl->recognizer) return;
    
    const int16_t* samples = reinterpret_cast<const int16_t*>(audioData.constData());
    int numSamples = audioData.size() / sizeof(int16_t);
    
    std::vector<float> floatSamples(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        floatSamples[i] = samples[i] / 32768.0f;
    }
    
    auto stream = m_impl->recognizer->CreateStream();
    stream->AcceptWaveform(sampleRate, floatSamples);
    m_streams.push_back(std::move(stream));
}

QString SenseVoiceRecognizer::finalResult() {
    if (!m_impl->recognizer || m_streams.empty()) {
        return QString();
    }
    
    for (auto& stream : m_streams) {
        m_impl->recognizer->DecodeStream(stream.get());
    }
    
    QString result;
    for (auto& stream : m_streams) {
        auto r = stream->GetResult();
        if (!r.text.empty()) {
            if (!result.isEmpty()) result += " ";
            result += QString::fromStdString(r.text);
        }
    }
    
    m_streams.clear();
    return result;
}

void SenseVoiceRecognizer::reset() {
    m_streams.clear();
}

void SenseVoiceRecognizer::setHotwords(const QStringList& words, float score) {
    // TODO: реализовать через config.hotwords_file в sherpa-onnx
    Q_UNUSED(words)
    Q_UNUSED(score)
}

bool SenseVoiceRecognizer::isLoaded() const {
    return m_loaded;
}
