#pragma once

#include <QList>
#include <QString>

// Каталог скачиваемых моделей. Вшит в код намеренно: модели обновляются
// редко, а серверная инфраструктура (манифесты, зеркала) проекту не нужна.
// URL — официальные релизы sherpa-onnx (тот же источник, что и в docs/models.md,
// откуда список и сверялся побайтово: размер + sha256).
//
// Если модель всё же заменят на релизе (что ломает sha256) — загрузчик честно
// сообщит о несовпадении контрольной суммы и не подсунет модели битый файл.

// Одна скачиваемая позиция: VAD или архив ASR-профиля.
struct ModelPackage {
    QString id;           // короткое имя для CLI: "zipformer-ru"
    QString title;        // для мастера загрузки
    QString description;  // одна-две строки: зачем эта модель
    QString fileName;     // что скачивается: "silero_vad.onnx" или "*.tar.bz2"
    QString url;
    QString sha256;       // контрольная сумма ФАЙЛА по ссылке
    qint64  sizeBytes = 0;
    bool    required = false;   // VAD нужен всегда
    QString profileName;        // профиль [asr_*], который оживёт; пуста для VAD

    bool isArchive() const { return fileName.endsWith(QLatin1String(".tar.bz2")); }
    // Каталог, который появится после распаковки архива (имя файла без .tar.bz2)
    QString extractedDirName() const;
    // Имя файла/каталога, наличие которого в destDir означает «установлено»
    QString installedName() const;
};

// Весь каталог (VAD первым).
const QList<ModelPackage>& modelCatalog();

// Пакет по id; nullptr, если такого нет.
const ModelPackage* findModelPackage(const QString& id);

// Пакет установлен в destDir: для файла — существует; для архива — конечный
// каталог существует И не пуст (пустой каталог — след обрыва распаковки в
// старых версиях; атомарный rename такого больше не оставит, но чужие
// недописанные каталоги проверять всё равно надо).
bool isModelPackageInstalled(const ModelPackage& p, const QString& destDir);

// Какие пакеты ещё не установлены в destDir.
QList<ModelPackage> missingModelPackages(const QString& destDir);

// Рекомендованный ASR-профиль для авто-выбора после загрузки:
// первый установленный в порядке zipformer-ru, gigaam-v3-ctc, gigaam-v3,
// whisper-base. Пусто, если не установлено ничего.
QString recommendedInstalledProfile(const QString& destDir);
