#include "models/ModelCatalog.h"

#include <QDir>
#include <QFileInfo>

namespace {

const char* kReleaseBase =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/";

// Данные сверены фактическим скачиванием 2026-10-04: размер в байтах + sha256.
QList<ModelPackage> buildCatalog()
{
    QList<ModelPackage> c;

    ModelPackage vad;
    vad.id          = QStringLiteral("silero-vad");
    vad.title       = QStringLiteral("Silero VAD (обязательно)");
    vad.description = QStringLiteral("Детектор речи: режет поток на фразы. Без него "
                                      "не работает ничего. 0.6 МБ.");
    vad.fileName    = QStringLiteral("silero_vad.onnx");
    vad.url         = QLatin1String(kReleaseBase) + vad.fileName;
    vad.sha256      = QStringLiteral("9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6");
    vad.sizeBytes   = 643854;
    vad.required    = true;
    c << vad;

    ModelPackage zipformer;
    zipformer.id          = QStringLiteral("zipformer-ru");
    zipformer.title       = QStringLiteral("zipformer-ru — основная (рекомендуется)");
    zipformer.description = QStringLiteral("Русский transducer, 105 МБ. Быстрая "
                                            "(RTF ~0.06 на 2 потоках), единственный профиль, "
                                            "где подсказки работают обычными словами.");
    zipformer.fileName    = QStringLiteral("sherpa-onnx-small-zipformer-ru-2024-09-18.tar.bz2");
    zipformer.url         = QLatin1String(kReleaseBase) + zipformer.fileName;
    zipformer.sha256      = QStringLiteral("f766cea7df7e7204a5eac9edfba2d05e7546523a88e803f4a110b1865358d51a");
    zipformer.sizeBytes   = 109906654;
    zipformer.profileName = QStringLiteral("zipformer-ru");
    c << zipformer;

    ModelPackage gigaamCtc;
    gigaamCtc.id          = QStringLiteral("gigaam-v3-ctc");
    gigaamCtc.title       = QStringLiteral("GigaAM v3 CTC");
    gigaamCtc.description = QStringLiteral("156 МБ. Чуть быстрее RNN-T, но не поддерживает "
                                            "подсказки (hotwords).");
    gigaamCtc.fileName    = QStringLiteral("sherpa-onnx-nemo-ctc-giga-am-v3-russian-2025-12-16.tar.bz2");
    gigaamCtc.url         = QLatin1String(kReleaseBase) + gigaamCtc.fileName;
    gigaamCtc.sha256      = QStringLiteral("e1291d704460cab4a01716081170c86c12f6b15338a1534f71cc5956922adb52");
    gigaamCtc.sizeBytes   = 163286197;
    gigaamCtc.profileName = QStringLiteral("gigaam-v3-ctc");
    c << gigaamCtc;

    ModelPackage gigaam;
    gigaam.id          = QStringLiteral("gigaam-v3");
    gigaam.title       = QStringLiteral("GigaAM v3 RNN-T — для шумного аудио");
    gigaam.description = QStringLiteral("160 МБ. Устойчивее к шуму и нетипичной речи, "
                                         "но примерно в 5 раз медленнее zipformer-ru.");
    gigaam.fileName    = QStringLiteral("sherpa-onnx-nemo-transducer-giga-am-v3-russian-2025-12-16.tar.bz2");
    gigaam.url         = QLatin1String(kReleaseBase) + gigaam.fileName;
    gigaam.sha256      = QStringLiteral("20a439491904b839f35eb8efaa3d99cbfaaad0c6dcba22d06ff218bb0056772d");
    gigaam.sizeBytes   = 167388020;
    gigaam.profileName = QStringLiteral("gigaam-v3");
    c << gigaam;

    ModelPackage whisper;
    whisper.id          = QStringLiteral("whisper-base");
    whisper.title       = QStringLiteral("Whisper base — мультиязычная");
    whisper.description = QStringLiteral("198 МБ. Сама ставит пунктуацию и заглавные, "
                                          "знает английский, но русский заметно хуже "
                                          "и самая медленная (RTF ~0.51).");
    whisper.fileName    = QStringLiteral("sherpa-onnx-whisper-base.tar.bz2");
    whisper.url         = QLatin1String(kReleaseBase) + whisper.fileName;
    whisper.sha256      = QStringLiteral("911b2083efd7c0dca2ac3b358b75222660dc09fb716d64fbfc417ba6c99ff3de");
    whisper.sizeBytes   = 207557382;
    whisper.profileName = QStringLiteral("whisper-base");
    c << whisper;

    return c;
}

}  // namespace

QString ModelPackage::extractedDirName() const
{
    QString n = fileName;
    if (n.endsWith(QLatin1String(".tar.bz2"))) {
        n.chop(int(sizeof(".tar.bz2")) - 1);
    }
    return n;
}

QString ModelPackage::installedName() const
{
    return isArchive() ? extractedDirName() : fileName;
}

const QList<ModelPackage>& modelCatalog()
{
    static const QList<ModelPackage> catalog = buildCatalog();
    return catalog;
}

const ModelPackage* findModelPackage(const QString& id)
{
    for (const ModelPackage& p : modelCatalog()) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

bool isModelPackageInstalled(const ModelPackage& p, const QString& destDir)
{
    const QString path = QDir(destDir).filePath(p.installedName());
    if (!p.isArchive()) {
        return QFileInfo::exists(path);
    }
    const QDir dir(path);
    return dir.exists() && !dir.entryList(QDir::AllEntries | QDir::Hidden).isEmpty();
}

QList<ModelPackage> missingModelPackages(const QString& destDir)
{
    QList<ModelPackage> out;
    for (const ModelPackage& p : modelCatalog()) {
        if (!isModelPackageInstalled(p, destDir)) {
            out << p;
        }
    }
    return out;
}

QString recommendedInstalledProfile(const QString& destDir)
{
    // Порядок предпочтения совпадает с docs/models.md: основная первая.
    static const QStringList kPreference = {
        QStringLiteral("zipformer-ru"),
        QStringLiteral("gigaam-v3-ctc"),
        QStringLiteral("gigaam-v3"),
        QStringLiteral("whisper-base"),
    };
    for (const QString& id : kPreference) {
        const ModelPackage* p = findModelPackage(id);
        if (p && isModelPackageInstalled(*p, destDir)) {
            return p->profileName;
        }
    }
    return QString();
}
