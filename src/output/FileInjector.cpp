#include "output/FileInjector.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {

// Читаем/пишем байтами в UTF-8 намеренно: QTextStream с узким литералом
// кодирует его как Latin-1 (грабля №6 в development-log.md), а здесь через
// файл проходит кириллица.
QByteArray readAllUtf8(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return f.readAll();
}

}  // namespace

FileInjector::FileInjector() = default;

FileInjector::FileInjector(const Options& options)
    : m_options(options)
{
}

FileInjector::~FileInjector() = default;

QString FileInjector::dailyFileName(const QDate& date, bool markdown)
{
    return date.toString(QStringLiteral("yyyy-MM-dd"))
        + (markdown ? QStringLiteral(".md") : QStringLiteral(".txt"));
}

QString FileInjector::formatEntry(const QString& text,
                                 const QString& timestampFormat,
                                 bool markdown,
                                 const QDateTime& when)
{
    QString line;
    if (markdown) {
        line += QStringLiteral("- ");
    }
    if (!timestampFormat.isEmpty()) {
        line += when.toString(timestampFormat);
        line += QStringLiteral(" — ");   // длинное тире, не дефис: так заметку
                                         // визуально проще отличить от текста
    }
    line += text;
    return line;
}

QString FileInjector::eraseLastWord(const QString& line)
{
    QString s = line;

    // Как в редакторе: если строка кончается пробелами, Ctrl+BackSpace сначала
    // съедает их, и только потом — слово.
    if (!s.isEmpty() && s.at(s.size() - 1).isSpace()) {
        while (!s.isEmpty() && s.at(s.size() - 1).isSpace()) {
            s.chop(1);
        }
        return s;
    }

    int cut = s.size();
    while (cut > 0 && !s.at(cut - 1).isSpace()) {
        --cut;
    }
    if (cut == 0) {
        return QString();          // слово было одно
    }
    // Пробел перед стёртым словом тоже съедаем — иначе в файле копятся
    // хвосты. Исключение: маркер списка, «- слово» должно стать «- »,
    // а не «-».
    const QString head = s.left(cut).trimmed();
    if (head == QLatin1Char('-') || head == QLatin1Char('*')) {
        s.truncate(cut);
    } else {
        s.truncate(cut - 1);
    }
    return s;
}

QString FileInjector::filePath() const
{
    if (!m_options.file.isEmpty()) {
        return m_options.file;
    }
    const QString name = dailyFileName(QDate::currentDate(), m_options.markdown);
    return m_options.dir.isEmpty() ? name : QDir(m_options.dir).filePath(name);
}

bool FileInjector::ensureDir()
{
    if (!m_options.file.isEmpty()) {
        const QFileInfo fi(m_options.file);
        const QString parent = fi.absolutePath();
        if (!QDir().mkpath(parent)) {
            m_lastError = QStringLiteral("не удалось создать каталог %1").arg(parent);
            return false;
        }
        return true;
    }
    if (m_options.dir.isEmpty()) {
        m_lastError = QStringLiteral("не задан ни [notes] file, ни [notes] dir");
        return false;
    }
    if (!QDir().mkpath(m_options.dir)) {
        m_lastError = QStringLiteral("не удалось создать каталог заметок %1").arg(m_options.dir);
        return false;
    }
    return true;
}

bool FileInjector::initialize()
{
    m_lastError.clear();

    if (!ensureDir()) {
        qWarning().noquote() << QStringLiteral("FileInjector: %1").arg(m_lastError);
        return false;
    }

    // Проверяем права сразу, а не в момент первой заметки: иначе «заметка»
    // промолчит в самый неподходящий момент.
    const QString path = filePath();
    QFile probe(path);
    if (!probe.open(QIODevice::Append | QIODevice::Text)) {
        m_lastError = QStringLiteral("файл заметок %1 недоступен для записи: %2")
                          .arg(path, probe.errorString());
        qWarning().noquote() << QStringLiteral("FileInjector: %1").arg(m_lastError);
        return false;
    }
    probe.close();

    m_ready = true;
    qInfo().noquote() << QStringLiteral("FileInjector: заметки -> %1 (метка времени: %2)")
                             .arg(path, m_options.timestampFormat.isEmpty()
                                            ? QStringLiteral("нет")
                                            : m_options.timestampFormat);
    return true;
}

bool FileInjector::isAvailable() const
{
    return m_ready;
}

QString FileInjector::backendName() const
{
    return QStringLiteral("файл заметок");
}

bool FileInjector::appendLine(const QString& line)
{
    if (!m_ready) {
        m_lastError = QStringLiteral("FileInjector не инициализирован");
        return false;
    }

    const QString path = filePath();
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) {
        m_lastError = QStringLiteral("не удалось открыть %1: %2").arg(path, f.errorString());
        qWarning().noquote() << QStringLiteral("FileInjector: %1").arg(m_lastError);
        return false;
    }

    // Заголовок дня — только в новый файл и только для markdown-заметок
    const bool fresh = (f.size() == 0);
    if (fresh && m_options.dayHeader && m_options.markdown && m_options.file.isEmpty()) {
        const QString header = QStringLiteral("# Заметки %1\n\n")
                                   .arg(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
        f.write(header.toUtf8());
    }

    f.write(line.toUtf8());
    f.write("\n");
    f.close();

    if (!line.isEmpty()) {
        ++m_entries;
    }
    return true;
}

QString FileInjector::lastLine() const
{
    const QString content = QString::fromUtf8(readAllUtf8(filePath()));
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (!lines.at(i).trimmed().isEmpty()) {
            return lines.at(i);
        }
    }
    return QString();
}

bool FileInjector::rewriteLastLine(const QString& newLine)
{
    if (!m_ready) {
        m_lastError = QStringLiteral("FileInjector не инициализирован");
        return false;
    }

    const QString path = filePath();
    const QString content = QString::fromUtf8(readAllUtf8(path));
    if (content.isEmpty()) {
        return true;      // стирать нечего — не ошибка
    }

    const QStringList lines = content.split(QLatin1Char('\n'));
    int target = -1;
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (!lines.at(i).trimmed().isEmpty()) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        return true;
    }

    QStringList updated = lines;
    updated[target] = newLine;

    // QSaveFile: заметки пишутся по одной строке, и обрывать файл на середине
    // из-за сбоя питания было бы обидно.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("не удалось перезаписать %1: %2").arg(path, out.errorString());
        qWarning().noquote() << QStringLiteral("FileInjector: %1").arg(m_lastError);
        return false;
    }
    out.write(updated.join(QLatin1Char('\n')).toUtf8());
    if (!out.commit()) {
        m_lastError = QStringLiteral("не удалось сохранить %1: %2").arg(path, out.errorString());
        return false;
    }
    return true;
}

bool FileInjector::typeText(const QString& text)
{
    if (text.isEmpty()) {
        return true;      // пустой сегмент VAD — не ошибка
    }

    // ASR иногда отдаёт текст с переводами строк (Whisper). Каждая строка
    // становится отдельной заметкой, иначе markdown-список поедет.
    const QStringList parts = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    bool ok = true;
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (!appendLine(formatEntry(trimmed, m_options.timestampFormat, m_options.markdown))) {
            ok = false;
            break;
        }
    }
    return ok;
}

bool FileInjector::sendKey(const QString& key)
{
    const QString k = key.trimmed().toLower();

    if (k == QLatin1String("return") || k == QLatin1String("enter")
        || k == QLatin1String("new line") || k == QLatin1String("newline")) {
        return appendLine(QString());      // пустая строка = разделение абзацев
    }

    if (k == QLatin1String("backspace") || k == QLatin1String("ctrl+backspace")) {
        const QString line = lastLine();
        if (line.isEmpty()) {
            return true;                   // стирать нечего
        }
        return rewriteLastLine(eraseLastWord(line));
    }

    if (k == QLatin1String("space")) {
        const QString line = lastLine();
        if (line.isEmpty()) {
            return true;
        }
        return rewriteLastLine(line + QLatin1Char(' '));
    }

    // Остальные клавиши («shift+Home», «ctrl+BackSpace» в сочетании с чем-либо)
    // к файлу неприменимы. Не ошибка: команда просто не имеет смысла в этой цели.
    qDebug().noquote() << QStringLiteral("FileInjector: клавиша '%1' для файла заметок "
                                        "не применяется, пропущена").arg(key);
    return true;
}
