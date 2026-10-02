#include "QtAudioCapture.h"
#include <QAudioDeviceInfo>
#include <QDebug>

QtAudioCapture::QtAudioCapture(QObject* parent)
    : IAudioCapture(parent)
{
}

QtAudioCapture::~QtAudioCapture() {
    stop();
}

bool QtAudioCapture::initialize() {
    QAudioFormat format;
    format.setSampleRate(m_sampleRate);
    format.setChannelCount(1);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setSampleType(QAudioFormat::SignedInt);
    format.setByteOrder(QAudioFormat::LittleEndian);

    QAudioDeviceInfo deviceInfo = QAudioDeviceInfo::defaultInputDevice();
    
    if (!deviceInfo.isFormatSupported(format)) {
        qWarning() << "Default format not supported, trying nearest";
        format = deviceInfo.nearestFormat(format);
        
        if (format.sampleRate() != m_sampleRate || format.channelCount() != 1) {
            emit errorOccurred("Audio device does not support required format");
            return false;
        }
    }

    m_audioInput = new QAudioInput(deviceInfo, format, this);
    
    // Уменьшаем буфер до ~50 мс (16000 Hz * 0.05 s * 2 bytes = 1600 bytes)
    m_audioInput->setBufferSize(1600);

    connect(m_audioInput, &QAudioInput::stateChanged,
            this, &QtAudioCapture::onStateChanged);

    qDebug() << "QtAudioCapture initialized: sampleRate=" << format.sampleRate()
             << "bufferSize=" << m_audioInput->bufferSize();

    return true;
}

void QtAudioCapture::start() {
    if (m_running) {
        return;
    }

    if (!m_audioInput) {
        emit errorOccurred("QtAudioCapture not initialized");
        return;
    }

    m_device = m_audioInput->start();
    
    if (!m_device) {
        emit errorOccurred("Failed to start audio input");
        return;
    }

    bool connected = connect(m_device, &QIODevice::readyRead,
                             this, &QtAudioCapture::onAudioDataReady);
    
    if (!connected) {
        m_audioInput->stop();
        emit errorOccurred("Failed to connect audio signal");
        return;
    }

    m_running = true;
    qDebug() << "QtAudioCapture started";
}

void QtAudioCapture::stop() {
    if (!m_running) {
        return;
    }

    if (m_device) {
        disconnect(m_device, &QIODevice::readyRead,
                   this, &QtAudioCapture::onAudioDataReady);
        m_device = nullptr;
    }

    if (m_audioInput) {
        m_audioInput->stop();
    }

    m_running = false;
    qDebug() << "QtAudioCapture stopped";
}

bool QtAudioCapture::isRunning() const {
    return m_running;
}

void QtAudioCapture::onAudioDataReady() {
    if (!m_device || !m_running) {
        return;
    }

    QByteArray data = m_device->readAll();
    
    if (!data.isEmpty()) {
        emit audioDataReady(data, m_sampleRate);
    }
}

void QtAudioCapture::onStateChanged(QAudio::State state) {
    if (!m_audioInput) {
        return;
    }
    
    QAudio::Error error = m_audioInput->error();
    
    if (error != QAudio::NoError) {
        QString errorMsg;
        switch (error) {
            case QAudio::OpenError:
                errorMsg = "Open error";
                break;
            case QAudio::IOError:
                errorMsg = "IO error";
                break;
            case QAudio::UnderrunError:
                errorMsg = "Underrun error";
                break;
            case QAudio::FatalError:
                errorMsg = "Fatal error";
                break;
            default:
                errorMsg = "Unknown error";
                break;
        }
        
        qCritical() << "Audio error:" << errorMsg;
        emit errorOccurred(errorMsg);
    }
    
    qDebug() << "Audio state:" << state;
}