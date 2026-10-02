#pragma once

#include <QString>
#include <memory>

#include "config/AsrProfile.h"

class IRecognizer;
class QObject;

// Создаёт конкретный распознаватель по профилю из конфига.
// Единственное место, где приложение узнаёт, какая именно модель подключена —
// дальше всё работает через интерфейс IRecognizer.
namespace RecognizerFactory {

std::unique_ptr<IRecognizer> create(const AsrProfile& profile,
                                    QObject* parent = nullptr,
                                    QString* error = nullptr);

}  // namespace RecognizerFactory
