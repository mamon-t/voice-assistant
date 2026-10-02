// config/ConfigManager.h
class ConfigManager {
public:
    QString modelsPath() const { 
        return QDir::home().filePath(".voice_models"); 
    }
    
    QString senseVoiceModelPath() const {
        return modelsPath() + "/sense-voice-small/model.int8.onnx";
    }
    
    QString senseVoiceTokensPath() const {
        return modelsPath() + "/sense-voice-small/tokens.txt";
    }
    
    QString sileroVadPath() const {
        return modelsPath() + "/silero_vad.onnx";
    }
};