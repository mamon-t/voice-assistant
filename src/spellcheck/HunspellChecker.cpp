#include "spellcheck/HunspellChecker.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>

#ifdef HAVE_HUNSPELL
// Намеренно чистый C API, а не hunspell.hxx.
//
// C++ API hunspell принимает std::string, то есть зависит от _GLIBCXX_USE_CXX11_ABI.
// Если проект собран со старым ABI (опция SHERPA_ONNX_OLD_CXX_ABI под предсобранные
// sherpa-onnx), а системный libhunspell — с новым, линковка падает с
//   undefined reference to `Hunspell::spell(std::string const&, int*, std::string*)'
// C API работает с char* и от ABI не зависит вообще. Та же причина, по которой
// sherpa-onnx предоставляет c-api.h отдельно от cxx-api.h.
#include <hunspell/hunspell.h>
#endif

// ---------------------------------------------------------------------------
// Pimpl: без HAVE_HUNSPELL класс остаётся компилируемым, но нерабочим
// ---------------------------------------------------------------------------

#ifdef HAVE_HUNSPELL

struct HunspellChecker::Impl {
    Hunhandle* handle = nullptr;

    ~Impl()
    {
        if (handle) {
            Hunspell_destroy(handle);
            handle = nullptr;
        }
    }
};

#else

struct HunspellChecker::Impl {};

#endif

namespace {

// Слова из одних цифр («2026») словарь не знает — пропускаем их явно,
// иначе каждое число в диктовке считалось бы ошибкой.
bool isNumeric(const QString& w)
{
    if (w.isEmpty()) {
        return false;
    }
    for (const QChar& c : w) {
        if (!c.isDigit()) {
            return false;
        }
    }
    return true;
}

}  // namespace

HunspellChecker::HunspellChecker() = default;

HunspellChecker::HunspellChecker(const Options& options)
    : m_options(options)
{
}

HunspellChecker::~HunspellChecker() = default;

QStringList HunspellChecker::standardDictionaryDirs()
{
    return {
        QStringLiteral("/usr/share/hunspell"),
        QStringLiteral("/usr/share/myspell"),
        QStringLiteral("/usr/local/share/hunspell"),
        QDir::home().filePath(QStringLiteral(".hunspell")),
        QDir::home().filePath(QStringLiteral(".local/share/hunspell")),
    };
}

QStringList HunspellChecker::splitWords(const QString& text)
{
    // \w с включённым Unicode содержит цифры и '_', поэтому берём только буквы:
    // [^\W\d_] == «буква»
    static const QRegularExpression re(QStringLiteral("[^\\W\\d_]+"),
                                       QRegularExpression::UseUnicodePropertiesOption);

    QStringList words;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString w = it.next().captured();
        if (!w.isEmpty()) {
            words << w;
        }
    }
    return words;
}

bool HunspellChecker::initialize()
{
    if (m_ready) {
        return true;
    }

#ifndef HAVE_HUNSPELL
    m_lastError = QStringLiteral(
        "Проект собран без поддержки hunspell. Поставьте libhunspell-dev и словарь "
        "(sudo apt install libhunspell-dev hunspell-ru) и пересоберите: "
        "cd build && cmake .. && make -j4");
    qWarning().noquote() << "HunspellChecker:" << m_lastError;
    return false;
#else
    // --- ищем словарь ---
    QStringList dirs;
    if (!m_options.dictionaryDir.isEmpty()) {
        dirs << m_options.dictionaryDir;
    }
    dirs << standardDictionaryDirs();

    for (const QString& dir : dirs) {
        const QString dic = QDir(dir).filePath(m_options.lang + QStringLiteral(".dic"));
        const QString aff = QDir(dir).filePath(m_options.lang + QStringLiteral(".aff"));
        if (QFileInfo::exists(dic) && QFileInfo::exists(aff)) {
            m_dicPath = dic;
            m_affPath = aff;
            break;
        }
    }

    if (m_dicPath.isEmpty()) {
        m_lastError = QStringLiteral(
            "Словарь '%1' не найден. Искали в: %2\n"
            "  Установка: sudo apt install hunspell-ru   (или hunspell-en-us и т. п.)\n"
            "  Либо укажите каталог: [spellcheck] dictionary_dir=…")
                .arg(m_options.lang, dirs.join(QStringLiteral(", ")));
        qWarning().noquote() << "HunspellChecker:" << m_lastError;
        return false;
    }

    m_impl = std::make_unique<Impl>();
    m_impl->handle = Hunspell_create(m_affPath.toLocal8Bit().constData(),
                                     m_dicPath.toLocal8Bit().constData());
    if (!m_impl->handle) {
        m_lastError = QStringLiteral("Hunspell_create не смог загрузить %1 / %2")
                          .arg(m_affPath, m_dicPath);
        qWarning().noquote() << "HunspellChecker:" << m_lastError;
        m_impl.reset();
        return false;
    }

    // Словарь обязан быть в UTF-8: текст к нам приходит из ASR именно в UTF-8,
    // а hunspell сравнивает байты как есть. Словари в KOI8-R/ISO8859-5 встречаются.
    char* encoding = Hunspell_get_dic_encoding(m_impl->handle);
    const QString enc = encoding ? QString::fromLatin1(encoding) : QStringLiteral("(не задана)");
    const bool utf8 = (QString::compare(enc, QStringLiteral("UTF-8"), Qt::CaseInsensitive) == 0)
                   || (QString::compare(enc, QStringLiteral("UTF8"),  Qt::CaseInsensitive) == 0);
    if (!utf8) {
        m_lastError = QStringLiteral(
            "Словарь %1 в кодировке '%2', а нужен UTF-8 — проверка русского текста "
            "не будет работать. Поставьте словарь в UTF-8 (в Debian/Ubuntu пакет "
            "hunspell-ru именно такой).").arg(m_dicPath, enc);
        qWarning().noquote() << "HunspellChecker:" << m_lastError;
        Hunspell_destroy(m_impl->handle);
        m_impl.reset();
        m_dicPath.clear();
        return false;
    }

    m_ready = true;
    qInfo().noquote() << QStringLiteral("HunspellChecker: словарь %1 (%2, %3)")
                             .arg(m_dicPath, m_options.lang, enc);
    return true;
#endif
}

QStringList HunspellChecker::check(const QString& text)
{
    QStringList errors;
#ifndef HAVE_HUNSPELL
    Q_UNUSED(text)
    return errors;
#else
    if (!m_ready || !m_impl || !m_impl->handle) {
        return errors;
    }

    const QStringList words = splitWords(text);
    for (const QString& w : words) {
        if (isNumeric(w)) {
            continue;
        }
        // Hunspell_spell: 0 = слово с ошибкой, не 0 = корректное
        if (Hunspell_spell(m_impl->handle, w.toUtf8().constData()) != 0) {
            continue;
        }
        if (!errors.contains(w)) {
            errors << w;   // без повторов, в порядке появления
        }
    }
    return errors;
#endif
}

QStringList HunspellChecker::suggest(const QString& word)
{
    QStringList out;
#ifndef HAVE_HUNSPELL
    Q_UNUSED(word)
    return out;
#else
    if (!m_ready || !m_impl || !m_impl->handle) {
        return out;
    }

    char** list = nullptr;
    const int n = Hunspell_suggest(m_impl->handle, &list, word.toUtf8().constData());
    if (n > 0 && list) {
        const int limit = (m_options.maxSuggestions > 0)
            ? qMin(n, m_options.maxSuggestions)
            : n;
        for (int i = 0; i < limit; ++i) {
            if (list[i]) {
                out << QString::fromUtf8(list[i]);
            }
        }
    }
    if (list) {
        Hunspell_free_list(m_impl->handle, &list, n);
    }
    return out;
#endif
}
