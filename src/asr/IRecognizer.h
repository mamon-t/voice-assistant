#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

// ЭТАЛОН интерфейса (такой же, как в твоём проекте).
class IRecognizer : public QObject {
    Q_OBJECT

public:
    explicit IRecognizer(QObject* parent = nullptr) : QObject(parent) {}
    ~IRecognizer() override = default;

    virtual bool initialize() = 0;
    virtual void acceptWaveform(const QByteArray& audioData, int sampleRate) = 0;
    virtual QString finalResult() = 0;
    virtual void reset() = 0;
    virtual void setHotwords(const QStringList& words, float score) = 0;
    virtual bool isLoaded() const = 0;

signals:
    void errorOccurred(const QString& message);
};
