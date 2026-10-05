#pragma once

#include <QDialog>

#include "audio/AudioFileDecoder.h"

class QComboBox;

// Параметры RAW-файла (PCM без контейнера): частота, каналы, кодирование.
//
// Показывается перед разбором, только если файл похож на RAW
// (AudioFileDecoder::isRawExtension или неизвестное расширение): из самого
// файла параметры прочитать некуда — их может задать только человек. Для
// телефонных записей типично 8000 Гц, моно, 16 бит или G.711 — они и стоят
// по умолчанию, а последний выбор запоминается в [transcribe] settings.ini.
class RawParamsDialog : public QDialog {
    Q_OBJECT

public:
    explicit RawParamsDialog(const AudioFileDecoder::RawParams& initial,
                             const QString& filePath,
                             QWidget* parent = nullptr);

    AudioFileDecoder::RawParams params() const;

private:
    QComboBox* m_rate     = nullptr;
    QComboBox* m_channels = nullptr;
    QComboBox* m_format   = nullptr;
};
