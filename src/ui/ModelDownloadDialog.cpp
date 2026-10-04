#include "ModelDownloadDialog.h"

#include "config/ConfigManager.h"
#include "models/ModelCatalog.h"
#include "models/ModelDownloader.h"

#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 КБ").arg(bytes / 1024.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 МБ").arg(bytes / (1024.0 * 1024.0), 0, 'f', 0);
}

enum Column { ColCheck = 0, ColModel, ColSize, ColStatus, ColCount };

}  // namespace

ModelDownloadDialog::ModelDownloadDialog(QWidget* parent)
    : QDialog(parent)
    , m_downloader(new ModelDownloader(this))
{
    const ConfigManager cfg;
    m_destDir = cfg.modelsPath();

    setWindowTitle(QStringLiteral("Модели распознавания"));
    resize(680, 420);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(
        QStringLiteral("Распознавание работает полностью офлайн, но модели нужно скачать один раз.\n"
                       "Каталог: %1\n"
                       "Файлы проверяются по sha256; недокачанное при обрыве докачается само.")
            .arg(m_destDir)));

    m_table = new QTableWidget(0, ColCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral(""), QStringLiteral("Модель"),
                                        QStringLiteral("Размер"), QStringLiteral("Состояние")});
    m_table->horizontalHeader()->setSectionResizeMode(ColModel, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    const auto& catalog = modelCatalog();
    m_table->setRowCount(catalog.size());
    for (int row = 0; row < catalog.size(); ++row) {
        const ModelPackage& p = catalog.at(row);

        auto* check = new QTableWidgetItem();
        check->setFlags(Qt::ItemIsUserCheckable | (p.required ? Qt::NoItemFlags
                                                              : Qt::ItemIsEnabled));
        check->setData(Qt::CheckStateRole, Qt::Unchecked);
        check->setToolTip(p.required ? QStringLiteral("Обязательная — скачивается всегда")
                                     : QString());
        m_table->setItem(row, ColCheck, check);

        auto* title = new QTableWidgetItem(p.title);
        title->setToolTip(p.description);
        title->setData(Qt::UserRole, p.id);
        m_table->setItem(row, ColModel, title);

        m_table->setItem(row, ColSize, new QTableWidgetItem(humanSize(p.sizeBytes)));
        m_table->setItem(row, ColStatus, new QTableWidgetItem(QString()));
    }
    lay->addWidget(m_table);

    lay->addWidget(new QLabel(QStringLiteral(
        "Описание модели — во всплывающей подсказке. Рекомендация: zipformer-ru;\n"
        "остальные стоит брать, когда её точности не хватает (см. docs/models.md).")));

    m_progress = new QProgressBar();
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    lay->addWidget(m_progress);

    m_statusLabel = new QLabel(QStringLiteral("Готов к загрузке."));
    m_statusLabel->setWordWrap(true);
    lay->addWidget(m_statusLabel);

    auto* btns = new QHBoxLayout();
    btns->addStretch();
    m_startBtn  = new QPushButton(QStringLiteral("Скачать отмеченные"));
    m_cancelBtn = new QPushButton(QStringLiteral("Отменить"));
    m_closeBtn  = new QPushButton(QStringLiteral("Закрыть"));
    m_cancelBtn->setEnabled(false);
    btns->addWidget(m_startBtn);
    btns->addWidget(m_cancelBtn);
    btns->addWidget(m_closeBtn);
    lay->addLayout(btns);

    connect(m_startBtn,  &QPushButton::clicked, this, &ModelDownloadDialog::onStart);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { m_downloader->cancel(); });
    connect(m_closeBtn,  &QPushButton::clicked, this, &QDialog::accept);

    connect(m_downloader, &ModelDownloader::progress,        this, &ModelDownloadDialog::onProgress);
    connect(m_downloader, &ModelDownloader::packageFinished, this, &ModelDownloadDialog::onPackageFinished);
    connect(m_downloader, &ModelDownloader::packageFailed,   this, &ModelDownloadDialog::onPackageFailed);
    connect(m_downloader, &ModelDownloader::finished,        this, &ModelDownloadDialog::onAllFinished);

    refreshInstalledState();
}

void ModelDownloadDialog::refreshInstalledState()
{
    // Уже установленные — отметить и не предлагать качать снова
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QString id = m_table->item(row, ColModel)->data(Qt::UserRole).toString();
        const ModelPackage* p = findModelPackage(id);
        if (!p) {
            continue;
        }
        if (isModelPackageInstalled(*p, m_destDir)) {
            m_table->item(row, ColCheck)->setCheckState(Qt::Unchecked);
            m_table->item(row, ColCheck)->setFlags(Qt::NoItemFlags);
            setStatus(id, QStringLiteral("✔ установлен"));
        } else {
            // обязательный VAD и рекомендованный профиль — отметить сразу:
            // путь «установил пакет -> открыл мастер -> нажал одну кнопку»
            if (p->required || p->id == QLatin1String("zipformer-ru")) {
                m_table->item(row, ColCheck)->setCheckState(Qt::Checked);
            }
        }
    }
}

int ModelDownloadDialog::rowForId(const QString& id) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, ColModel)->data(Qt::UserRole).toString() == id) {
            return row;
        }
    }
    return -1;
}

void ModelDownloadDialog::setStatus(const QString& id, const QString& status)
{
    const int row = rowForId(id);
    if (row >= 0) {
        m_table->item(row, ColStatus)->setText(status);
    }
}

void ModelDownloadDialog::onStart()
{
    QList<ModelPackage> selected;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const bool checked = m_table->item(row, ColCheck)->checkState() == Qt::Checked;
        const QString id = m_table->item(row, ColModel)->data(Qt::UserRole).toString();
        const ModelPackage* p = findModelPackage(id);
        if (checked && p) {
            selected << *p;
        }
    }
    // VAD обязателен всегда: без него не работает ни один профиль
    const ModelPackage* vad = findModelPackage(QStringLiteral("silero-vad"));
    if (vad && !isModelPackageInstalled(*vad, m_destDir)) {
        bool already = false;
        for (const ModelPackage& p : selected) {
            already |= (p.id == vad->id);
        }
        if (!already) {
            selected.prepend(*vad);
        }
    }
    if (selected.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("Отметьте хотя бы одну модель."));
        return;
    }

    m_startBtn->setEnabled(false);
    m_cancelBtn->setEnabled(true);
    m_closeBtn->setEnabled(false);
    m_progress->setValue(0);
    m_downloader->download(selected, m_destDir);
}

void ModelDownloadDialog::onProgress(const QString& id, qint64 received, qint64 total)
{
    const int pct = total > 0 ? static_cast<int>(received * 100 / total) : 0;
    m_progress->setValue(pct);
    m_statusLabel->setText(QStringLiteral("%1: %2 из %3 (%4%)")
                               .arg(id, humanSize(received), humanSize(total)).arg(pct));
}

void ModelDownloadDialog::onPackageFinished(const QString& id)
{
    setStatus(id, QStringLiteral("✔ установлен"));
}

void ModelDownloadDialog::onPackageFailed(const QString& id, const QString& error)
{
    setStatus(id, QStringLiteral("✖ %1").arg(error));
}

void ModelDownloadDialog::onAllFinished(bool allOk)
{
    m_startBtn->setEnabled(true);
    m_cancelBtn->setEnabled(false);
    m_closeBtn->setEnabled(true);

    if (allOk) {
        m_progress->setValue(100);
        m_statusLabel->setText(QStringLiteral("Готово. Всё скачанное проверено по sha256 и установлено."));
        const QString rec = recommendedInstalledProfile(m_destDir);
        if (!rec.isEmpty()) {
            emit modelsInstalled(rec);
        }
    } else {
        m_statusLabel->setText(QStringLiteral("Загрузка завершилась с ошибками или отменена. "
                                              "Недокачанные файлы сохранены (.part) — повторите, "
                                              "они докачаются с того же места."));
    }
    refreshInstalledState();
}

void ModelDownloadDialog::closeEvent(QCloseEvent* event)
{
    if (m_downloader->isRunning()) {
        m_downloader->cancel();   // .part останется, докачается в следующий раз
    }
    QDialog::closeEvent(event);
}
