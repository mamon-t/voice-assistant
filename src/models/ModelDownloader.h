#pragma once

#include "models/ModelCatalog.h"

#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QScopedPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Последовательная загрузка моделей в каталог (по умолчанию ~/.voice_models).
//
// Честность важнее скорости: каждый файл проверяется по sha256 из каталога
// ДО распаковки/переименования, поэтому битая или подменённая модель не
// окажется в каталоге моделей. Недокачанный файл остаётся как <имя>.part
// и докачивается с того же места (HTTP Range) — на 100–200 МБ это не роскошь.
//
// Поток один (последовательно), UI получает progress()/packageFinished()/
// packageFailed() и сам решает, что рисовать.
class ModelDownloader : public QObject {
    Q_OBJECT

public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    // Ставит пакеты в очередь и начинает; повторные вызовы игнорируются,
    // пока загрузка идёт. destDir создаётся при необходимости.
    // force=true — переустановить даже то, что считается установленным
    // (лечит каталоги, недописанные старой версией или руками).
    void download(const QList<ModelPackage>& packages, const QString& destDir,
                  bool force = false);
    void cancel();          // обрывает текущий файл (.part остаётся для докачки)
    bool isRunning() const { return m_running; }

    // sha256 файла; пусто, если не открылся. Вынесена для тестов.
    static QString sha256File(const QString& path);

signals:
    void progress(const QString& id, qint64 received, qint64 total);
    void packageFinished(const QString& id);            // проверен и установлен
    void packageFailed(const QString& id, const QString& error);
    void finished(bool allOk);                          // конец всей очереди

private:
    void startNext();
    void fail(const QString& error);
    void onReplyReadyRead();
    void onReplyFinished();
    // precomputedDigest — для пути «.part уже целый»: потоковый m_hash
    // в этом случае пуст, хэш файла посчитан отдельно.
    bool verifyAndInstall(const QString& precomputedDigest = QString());
    bool extractArchive(const QString& archivePath, const QString& destDir, QString* error);

    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_reply = nullptr;

    QList<ModelPackage> m_queue;
    ModelPackage m_current;
    QString m_destDir;
    QFile   m_file;                 // <dest>/<fileName>.part
    // Qt5: QCryptographicHash не копируется и не имеет reset() — пересоздаём
    QScopedPointer<QCryptographicHash> m_hash;
    qint64  m_received = 0;         // байт в .part (с учётом докачки)
    bool    m_running = false;
    bool    m_cancelled = false;
    bool    m_anyFailed = false;
    bool    m_resumed = false;      // текущий файл качается с Range
    bool    m_force = false;        // переустановить даже «установленное»
};
