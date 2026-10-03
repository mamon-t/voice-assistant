#include "TextPostProcessor.h"

#include <QRegularExpression>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace {

bool isSentenceEnd(const QChar& c)
{
    return c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?');
}

bool isPunctuationToken(const QString& s)
{
    if (s.isEmpty()) {
        return false;
    }
    for (const QChar& c : s) {
        if (!QStringLiteral(".,!?;:-—\n()").contains(c)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TextPostProcessor::TextPostProcessor()
    : m_options(Options())
{
    setRules(defaultRules());   // setRules() ещё и сортирует: длинные фразы первыми
}

TextPostProcessor::TextPostProcessor(const Options& options)
    : m_options(options)
{
    setRules(defaultRules());
}

QList<TextPostProcessor::Rule> TextPostProcessor::defaultRules()
{
    // Порядок в списке не важен: setRules() отсортирует по убыванию длины фразы.
    return {
        { QStringLiteral("вопросительный знак"),    QStringLiteral("?") },
        { QStringLiteral("знак вопроса"),           QStringLiteral("?") },
        { QStringLiteral("восклицательный знак"),   QStringLiteral("!") },
        // ASR иногда теряет слово "знак" — оставляем одиночные варианты страховкой
        { QStringLiteral("вопросительный"),         QStringLiteral("?") },
        { QStringLiteral("восклицательный"),        QStringLiteral("!") },
        { QStringLiteral("точка с запятой"),        QStringLiteral(";") },
        { QStringLiteral("новая строка"),           QStringLiteral("\n") },
        { QStringLiteral("перенос строки"),         QStringLiteral("\n") },
        { QStringLiteral("красная строка"),         QStringLiteral("\n") },
        { QStringLiteral("открывающая скобка"),     QStringLiteral("(") },
        { QStringLiteral("закрывающая скобка"),     QStringLiteral(")") },
        { QStringLiteral("двоеточие"),              QStringLiteral(":") },
        { QStringLiteral("запятая"),                QStringLiteral(",") },
        { QStringLiteral("точка"),                  QStringLiteral(".") },
        { QStringLiteral("абзац"),                  QStringLiteral("\n") },
        { QStringLiteral("тире"),                   QStringLiteral("—") },
        { QStringLiteral("дефис"),                  QStringLiteral("-") },
        { QStringLiteral("пробел"),                 QStringLiteral(" ") },
    };
}

void TextPostProcessor::setRules(const QList<Rule>& rules)
{
    m_rules = rules;
    std::stable_sort(m_rules.begin(), m_rules.end(),
                     [](const Rule& a, const Rule& b) {
        const int wa = a.first.count(QLatin1Char(' ')) + 1;
        const int wb = b.first.count(QLatin1Char(' ')) + 1;
        return wa > wb;   // длинные фразы первыми
    });
}

QStringList TextPostProcessor::punctuationPhrases() const
{
    QStringList out;
    out.reserve(m_rules.size());
    for (const Rule& r : m_rules) {
        if (r.second.trimmed().isEmpty()) {
            continue;   // пропускаем "пробел" -> " "
        }
        if (!out.contains(r.first)) {
            out << r.first;
        }
    }
    return out;
}

bool TextPostProcessor::needsLeadingSpace(const QString& previous, const QString& next)
{
    if (previous.isEmpty() || next.isEmpty()) {
        return false;
    }

    const QChar last = previous.at(previous.size() - 1);
    if (last.isSpace()) {
        return false;   // разделитель уже есть
    }

    const QChar first = next.at(0);
    if (first.isSpace()) {
        return false;
    }
    // "…слово" + ", а также…" -> пробел перед запятой не ставится
    static const QString noSpaceBefore = QStringLiteral(".,!?;:)\u00bb\"'\u201d");
    if (noSpaceBefore.contains(first)) {
        return false;
    }
    return true;
}

QString TextPostProcessor::process(const QString& raw) const
{
    QString text = raw.trimmed();
    if (text.isEmpty()) {
        return text;
    }

    if (m_options.voicePunctuation && !m_rules.isEmpty()) {
        text = applyVoicePunctuation(text);
    }
    if (m_options.collapseSpaces) {
        text = fixSpacing(text);
    }
    if (text.isEmpty()) {
        return text;
    }
    if (m_options.capitalizeSentences) {
        text = capitalizeSentences(text);
    }
    if (m_options.addFinalDot) {
        text = ensureFinalDot(text);
    }
    return text;
}

QString TextPostProcessor::applyVoicePunctuation(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList words = text.split(ws, Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return text;
    }

    int maxRuleWords = 1;
    for (const Rule& r : m_rules) {
        maxRuleWords = std::max(maxRuleWords, r.first.count(QLatin1Char(' ')) + 1);
    }

    QStringList out;
    out.reserve(words.size());

    int i = 0;
    while (i < words.size()) {
        bool matched = false;
        const int maxSpan = std::min(maxRuleWords, words.size() - i);
        for (int span = maxSpan; span >= 2 && !matched; --span) {
            QString phrase = words.mid(i, span).join(QLatin1Char(' ')).toLower();
            for (const Rule& r : m_rules) {
                if (r.first.count(QLatin1Char(' ')) + 1 != span) {
                    continue;   // правила отсортированы по убыванию, но проверим явно
                }
                if (r.first == phrase) {
                    out << r.second;
                    i += span;
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            continue;
        }
        const QString lower = words.at(i).toLower();
        bool single = false;
        for (const Rule& r : m_rules) {
            if (r.first == lower) {
                out << r.second;
                single = true;
                break;
            }
        }
        if (!single) {
            out << words.at(i);
        }
        ++i;
    }

    return out.join(QLatin1Char(' '));
}

QString TextPostProcessor::fixSpacing(const QString& text)
{
    QString out;
    out.reserve(text.size());

    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);

        if (c == QLatin1Char('\n')) {
            while (out.endsWith(QLatin1Char(' '))) {
                out.chop(1);
            }
            if (!out.endsWith(QLatin1Char('\n'))) {
                out.append(c);
            }
            while (i + 1 < text.size() && text.at(i + 1) == QLatin1Char(' ')) {
                ++i;
            }
            continue;
        }

        if (c == QLatin1Char(' ')) {
            // схлопываем повторяющиеся пробелы и не ставим пробел после "("
            if (out.isEmpty() || out.endsWith(QLatin1Char(' '))
                || out.endsWith(QLatin1Char('\n')) || out.endsWith(QLatin1Char('('))) {
                continue;
            }
            // не ставим пробел перед закрывающей пунктуацией
            if (i + 1 < text.size()) {
                const QChar n = text.at(i + 1);
                if (QStringLiteral(".,!?;:)").contains(n)) {
                    continue;
                }
            }
            out.append(c);
            continue;
        }

        // убираем пробел, который уже добавлен перед ")" — на всякий случай
        if (c == QLatin1Char(')') && out.endsWith(QLatin1Char(' '))) {
            out.chop(1);
        }
        // после "(" пробел не нужен
        if (out.endsWith(QLatin1Char('(')) && c == QLatin1Char(' ')) {
            continue;
        }
        out.append(c);
    }

    while (out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    return out.trimmed();
}

QString TextPostProcessor::capitalizeSentences(const QString& text)
{
    if (text.isEmpty()) {
        return text;
    }

    QString out = text;
    bool atSentenceStart = true;

    for (int i = 0; i < out.size(); ++i) {
        const QChar c = out.at(i);

        if (c == QLatin1Char(' ') || c == QLatin1Char('\n')) {
            if (c == QLatin1Char('\n')) {
                atSentenceStart = true;
            }
            continue;
        }

        if (atSentenceStart) {
            const QString upper = QString(c).toUpper();
            if (upper != QString(c)) {
                out.replace(i, 1, upper);
            }
            atSentenceStart = false;
        } else if (isSentenceEnd(c)) {
            atSentenceStart = true;
        }
    }

    return out;
}

QString TextPostProcessor::ensureFinalDot(const QString& text)
{
    QString t = text;
    while (!t.isEmpty() && (t.endsWith(QLatin1Char(' ')) || t.endsWith(QLatin1Char('\n')))) {
        t.chop(1);
    }
    if (t.isEmpty()) {
        return t;
    }
    const QChar last = t.at(t.size() - 1);
    if (QStringLiteral(".!?;:").contains(last)) {
        return t;
    }
    t.append(QLatin1Char('.'));
    return t;
}
