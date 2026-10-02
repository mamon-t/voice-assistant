#pragma once

#include <onnxruntime_cxx_api.h>

class OnnxEnvironment {
public:
    static OnnxEnvironment& instance();
    
    bool isInitialized() const { return m_initialized; }
    Ort::Env& env() { return *m_env; }
    
private:
    OnnxEnvironment();
    ~OnnxEnvironment();
    
    OnnxEnvironment(const OnnxEnvironment&) = delete;
    OnnxEnvironment& operator=(const OnnxEnvironment&) = delete;
    
    bool m_initialized = false;
    std::unique_ptr<Ort::Env> m_env;
};