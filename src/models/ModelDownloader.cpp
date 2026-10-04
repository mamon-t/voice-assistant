#include "models/ModelDownloader.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>

namespace {

qint64 kExtractTimeoutMs = 10 * 60 * 1000;   // распаковка 200 МБ на медленном диске

QString humanMB(qint64 bytes)
{
    return QStringLiteral("%1 МБ").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

}  // namespace

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_hash(new QCryptographicHash(QCryptographicHash::Sha256))
{
}

ModelDownloader::~ModelDownloader()
{
    if (m_reply) {
        m_reply->abort();
    }
}

void ModelDownloader::download(const QList<ModelPackage>& packages, const QString& destDir,
                               bool force)
{
    if (m_running || packages.isEmpty()) {
        return;
    }
    QDir().mkpath(destDir);
    m_queue      = packages;
    m_destDir    = destDir;
    m_running    = true;
    m_cancelled  = false;
    m_anyFailed  = false;
    m_force      = force;
    startNext();
}

void ModelDownloader::cancel()
{
    m_cancelled = true;
    if (m_reply) {
        m_reply->abort();   // доедем до onReplyFinished, там тихо остановимся
    }
}

void ModelDownloader::startNext()
{
    if (m_cancelled) {
        m_running = false;
        emit finished(false);
        return;
    }
    if (m_queue.isEmpty()) {
        m_running = false;
        emit finished(!m_anyFailed);
        return;
    }

    m_current = m_queue.takeFirst();

    // Уже установлено — не качаем повторно (если не заказана переустановка)
    const QString installedPath = QDir(m_destDir).filePath(m_current.installedName());
    if (!m_force && isModelPackageInstalled(m_current, m_destDir)) {
        emit packageFinished(m_current.id);
        startNext();
        return;
    }
    if (m_force && QFileInfo::exists(installedPath)) {
        qInfo().noquote()
            << QStringLiteral("Модели: --force — удаляю установленный %1 и качаю заново")
                   .arg(m_current.installedName());
        if (m_current.isArchive()) {
            QDir(installedPath).removeRecursively();
        } else {
            QFile::remove(installedPath);
        }
    }

    // Прошлый запуск скачал файл ЦЕЛИКОМ, но умер до установки (например,
    // между проверкой и распаковкой). Не перекачиваем заново: проверяем
    // имеющийся .part и ставим.
    const QString partPath = QDir(m_destDir).filePath(m_current.fileName + QStringLiteral(".part"));
    {
        const QFileInfo partInfo(partPath);
        if (!m_force && partInfo.exists() && partInfo.size() == m_current.sizeBytes) {
            const QString digest = sha256File(partPath);
            if (digest.compare(m_current.sha256, Qt::CaseInsensitive) == 0) {
                qInfo().noquote()
                    << QStringLiteral("Модели: %1 уже скачан целиком — устанавливаю "
                                      "без повторной загрузки").arg(m_current.id);
                // Хэш уже посчитан по файлу; потоковый m_hash для него пуст —
                // передаём готовый дайджест.
                if (verifyAndInstall(digest)) {
                    emit packageFinished(m_current.id);
                    startNext();
                }
                return;
            }
            QFile::remove(partPath);   // целый, но битый — только заново
        }
    }

    // Докачка: .part от прошлой попытки остаётся на диске специально
    // (partPath вычислен выше)
    m_resumed  = false;
    m_received = 0;
    m_hash.reset(new QCryptographicHash(QCryptographicHash::Sha256));

    QFileInfo partInfo(partPath);
    if (partInfo.exists() && partInfo.size() > 0
        && partInfo.size() < m_current.sizeBytes) {
        // Хэш считаем с уже скачанной части, чтобы не перекачивать её
        QFile part(partPath);
        if (part.open(QIODevice::ReadOnly)) {
            while (!part.atEnd()) {
                m_hash->addData(part.read(1 << 20));
            }
            m_received = partInfo.size();
            m_resumed  = true;
        }
    }

    m_file.setFileName(partPath);
    if (!m_file.open(m_resumed ? (QIODevice::WriteOnly | QIODevice::Append)
                               : (QIODevice::WriteOnly | QIODevice::Truncate))) {
        fail(QStringLiteral("не удалось открыть %1 для записи").arg(partPath));
        return;
    }

    QNetworkRequest req{QUrl(m_current.url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);   // релизы GitHub редиректят на CDN
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(60000);   // минута без движения — ошибка, будет докачка
#endif
    if (m_resumed) {
        req.setRawHeader("Range", QStringLiteral("bytes=%1-").arg(m_received).toLatin1());
    }

    qInfo().noquote()
        << QStringLiteral("Модели: качаю %1 (%2)%3 -> %4")
               .arg(m_current.id, humanMB(m_current.sizeBytes),
                    m_resumed ? QStringLiteral(", докачка с ") + humanMB(m_received) : QString(),
                    m_destDir);

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelDownloader::onReplyReadyRead);
    connect(m_reply, &QNetworkReply::finished,  this, &ModelDownloader::onReplyFinished);
    emit progress(m_current.id, m_received, m_current.sizeBytes);
}

void ModelDownloader::onReplyReadyRead()
{
    if (!m_reply) {
        return;
    }
    // Сервер мог проигнорировать Range (ответ 200 вместо 206) — тогда
    // докачка не имеет смысла: перезапускаем файл с нуля.
    if (m_resumed) {
        const int code = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code != 206) {
            m_resumed = false;
            disconnect(m_reply, nullptr, this, nullptr);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
            m_file.close();
            // startNext() перечитает .part? Нет — обнуляем его явно:
            m_file.remove();
            m_queue.prepend(m_current);
            startNext();
            return;
        }
    }
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty()) {
        return;
    }
    m_file.write(chunk);
    m_hash->addData(chunk);
    m_received += chunk.size();
    emit progress(m_current.id, m_received, m_current.sizeBytes);
}

void ModelDownloader::onReplyFinished()
{
    if (!m_reply) {
        return;
    }
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (m_cancelled) {
        m_file.close();          // .part остаётся — при следующем запуске докачается
        m_running = false;
        qInfo().noquote() << QStringLiteral("Модели: отменено (%1 останется в .part для докачки)")
                                 .arg(m_current.id);
        emit finished(false);
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        m_file.close();
        fail(reply->errorString());   // .part сохраняем для докачки
        return;
    }

    m_file.close();
    if (!verifyAndInstall()) {
        return;   // fail() уже вызван внутри
    }
    emit packageFinished(m_current.id);
    startNext();
}

bool ModelDownloader::verifyAndInstall(const QString& precomputedDigest)
{
    const QString partPath = QDir(m_destDir).filePath(m_current.fileName + QStringLiteral(".part"));

    if (QFileInfo(partPath).size() != m_current.sizeBytes) {
        fail(QStringLiteral("размер не совпал: скачано %1, ожидалось %2 — попробуйте ещё раз "
                            "(файл будет докачан)")
                 .arg(QFileInfo(partPath).size()).arg(m_current.sizeBytes));
        return false;
    }

    const QString digest = precomputedDigest.isEmpty()
        ? QString::fromLatin1(m_hash->result().toHex())
        : precomputedDigest;
    if (digest.compare(m_current.sha256, Qt::CaseInsensitive) != 0) {
        // Целостность важнее экономии трафика: битый файл удаляем, докачка
        // из повреждённых байтов смысла не имеет.
        QFile::remove(partPath);
        fail(QStringLiteral("контрольная сумма НЕ совпала (файл повреждён или на сервере "
                            "другая версия) — удалён; sha256=%1").arg(digest.left(12)));
        return false;
    }

    if (m_current.isArchive()) {
        QString err;
        if (!extractArchive(partPath, m_destDir, &err)) {
            QFile::remove(partPath);
            fail(err);
            return false;
        }
        QFile::remove(partPath);
        if (!QFileInfo::exists(QDir(m_destDir).filePath(m_current.installedName()))) {
            fail(QStringLiteral("распаковка завершилась, но каталог %1 не появился")
                     .arg(m_current.installedName()));
            return false;
        }
    } else {
        const QString target = QDir(m_destDir).filePath(m_current.fileName);
        QFile::remove(target);
        if (!QFile::rename(partPath, target)) {
            fail(QStringLiteral("не удалось переименовать %1 в %2").arg(partPath, target));
            return false;
        }
    }

    qInfo().noquote() << QStringLiteral("Модели: %1 установлен (sha256 совпал)")
                             .arg(m_current.id);
    return true;
}

bool ModelDownloader::extractArchive(const QString& archivePath, const QString& destDir,
                                     QString* error)
{
    // Атомарная установка: распаковываем во временный каталог, затем
    // переименовываем в финальный. Убитый процесс оставляет после себя
    // <имя>.part-dir, а НЕ «наполовину готовый» каталог, который
    // missingModelPackages() приняла бы за установленную модель
    // (проверено на живом обрыве: сиротский tar дописывал каталог уже
    // после смерти загрузчика).
    // Системный tar есть везде, а libarchive был бы лишней зависимостью пакета.
    const QString finalPath   = QDir(destDir).filePath(m_current.installedName());
    const QString stagingPath = finalPath + QStringLiteral(".part-dir");

    QDir(stagingPath).removeRecursively();
    if (!QDir().mkpath(stagingPath)) {
        if (error) {
            *error = QStringLiteral("не удалось создать временный каталог %1").arg(stagingPath);
        }
        return false;
    }

    QProcess proc;
    proc.setProgram(QStringLiteral("tar"));
    proc.setArguments({QStringLiteral("-x"), QStringLiteral("-j"),
                       QStringLiteral("-f"), archivePath,
                       QStringLiteral("-C"), stagingPath});
    proc.start();
    if (!proc.waitForStarted(10000)) {
        QDir(stagingPath).removeRecursively();
        if (error) {
            *error = QStringLiteral("tar не запускается (нужен bzip2: sudo apt install bzip2)");
        }
        return false;
    }
    if (!proc.waitForFinished(kExtractTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        QDir(stagingPath).removeRecursively();
        if (error) {
            *error = QStringLiteral("распаковка не завершилась за 10 минут");
        }
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString stderrText = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        QDir(stagingPath).removeRecursively();
        if (error) {
            *error = QStringLiteral("tar вернул %1: %2").arg(proc.exitCode()).arg(stderrText);
        }
        return false;
    }

    // Архивы моделей содержат один верхний каталог; если имя вдруг другое —
    // берём единственный каталог внутри staging.
    QString staged = QDir(stagingPath).filePath(m_current.extractedDirName());
    if (!QFileInfo(staged).isDir()) {
        const auto entries = QDir(stagingPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (entries.size() == 1) {
            staged = QDir(stagingPath).filePath(entries.first());
        }
    }
    if (!QFileInfo(staged).isDir()) {
        QDir(stagingPath).removeRecursively();
        if (error) {
            *error = QStringLiteral("в архиве не нашлось каталога модели");
        }
        return false;
    }

    QDir(finalPath).removeRecursively();   // чиним и недо-распакованный ранее каталог
    if (!QFile::rename(staged, finalPath)) {
        QDir(stagingPath).removeRecursively();
        if (error) {
            *error = QStringLiteral("не удалось переименовать %1 в %2").arg(staged, finalPath);
        }
        return false;
    }
    QDir(stagingPath).removeRecursively();
    return true;
}

void ModelDownloader::fail(const QString& error)
{
    qWarning().noquote() << QStringLiteral("Модели: %1 — %2").arg(m_current.id, error);
    m_anyFailed = true;
    emit packageFailed(m_current.id, error);
    startNext();
}

QString ModelDownloader::sha256File(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        hash.addData(f.read(1 << 20));
    }
    return QString::fromLatin1(hash.result().toHex());
}
