#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>

// ЭТАЛОН интерфейса (такой же, как в твоём проекте, плюс конструктор с parent —
// без него `: IVad(parent)` в SileroVad не компилируется).
// Сигналы объявлены ЗДЕСЬ и только здесь: в производном классе их
// переопределять нельзя (сигналы Qt не виртуальные).
class IVad : public QObject {
    Q_OBJECT

public:
    explicit IVad(QObject* parent = nullptr) : QObject(parent) {}
    ~IVad() override = default;

    virtual bool initialize() = 0;
    virtual void processAudio(const QByteArray& audioData, int sampleRate) = 0;
    virtual void reset() = 0;

signals:
    void speechStarted();
    void speechEnded();
    void errorOccurred(const QString& message);
};
