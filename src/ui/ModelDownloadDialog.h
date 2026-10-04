#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class ModelDownloader;
struct ModelPackage;

// Мастер загрузки моделей — то, что видит тестировщик после установки пакета:
// список профилей с размером и описанием, галочки, прогресс, проверка sha256.
// Открывается из меню лотка («Скачать модели...») и автоматически при первом
// запуске, если детектор речи (Silero VAD) не установлен.
//
// По успешной загрузке испускает modelsInstalled(профиль) — TrayIcon делает
// его активным, если текущий активный профиль не готов (модель не скачана),
// и перезагружает пайплайн БЕЗ перезапуска приложения.
class ModelDownloadDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModelDownloadDialog(QWidget* parent = nullptr);

signals:
    void modelsInstalled(const QString& recommendedProfile);

protected:
    void closeEvent(QCloseEvent* event) override;   // закрытие во время загрузки = отмена

private slots:
    void onStart();
    void onProgress(const QString& id, qint64 received, qint64 total);
    void onPackageFinished(const QString& id);
    void onPackageFailed(const QString& id, const QString& error);
    void onAllFinished(bool allOk);

private:
    int  rowForId(const QString& id) const;
    void setStatus(const QString& id, const QString& status);
    void refreshInstalledState();

    QTableWidget*  m_table;
    QProgressBar*  m_progress;
    QLabel*        m_statusLabel;
    QPushButton*   m_startBtn;
    QPushButton*   m_cancelBtn;
    QPushButton*   m_closeBtn;
    ModelDownloader* m_downloader;
    QString m_destDir;
};
