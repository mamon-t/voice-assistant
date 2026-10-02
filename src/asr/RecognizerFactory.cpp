#include "RecognizerFactory.h"

#include "asr/IRecognizer.h"
#include "asr/NemoCtcRecognizer.h"
#include "asr/TransducerRecognizer.h"
#include "asr/WhisperRecognizer.h"

namespace RecognizerFactory {

std::unique_ptr<IRecognizer> create(const AsrProfile& profile, QObject* parent, QString* error)
{
    QString localError;
    if (!profile.isValid(&localError)) {
        if (error) {
            *error = localError;
        }
        return nullptr;
    }

    switch (profile.engine) {

    case AsrProfile::Engine::Transducer: {
        TransducerRecognizer::Options opt;
        opt.decodingMethod = profile.decodingMethod;
        opt.maxActivePaths = profile.maxActivePaths;
        opt.numThreads     = profile.numThreads;
        opt.hotwordsScore  = profile.hotwordsScore;
        opt.bpeVocab       = profile.bpeVocabPath;
        opt.modelingUnit   = profile.modelingUnit;
        opt.debug          = profile.debug;
        return std::make_unique<TransducerRecognizer>(profile.encoderPath,
                                                      profile.decoderPath,
                                                      profile.joinerPath,
                                                      profile.tokensPath,
                                                      opt,
                                                      parent);
    }

    case AsrProfile::Engine::NemoCtc: {
        NemoCtcRecognizer::Options opt;
        opt.numThreads = profile.numThreads;
        opt.debug      = profile.debug;
        return std::make_unique<NemoCtcRecognizer>(profile.ctcModelPath,
                                                   profile.tokensPath,
                                                   opt,
                                                   parent);
    }

    case AsrProfile::Engine::Whisper: {
        WhisperRecognizer::Options opt;
        opt.language   = profile.language;
        opt.numThreads = profile.numThreads;
        opt.debug      = profile.debug;
        return std::make_unique<WhisperRecognizer>(profile.encoderPath,
                                                   profile.decoderPath,
                                                   profile.tokensPath,
                                                   opt,
                                                   parent);
    }

    case AsrProfile::Engine::Unknown:
        break;
    }

    if (error) {
        *error = localError.isEmpty()
            ? QStringLiteral("профиль '%1': неподдерживаемый engine").arg(profile.name)
            : localError;
    }
    return nullptr;
}

}  // namespace RecognizerFactory
