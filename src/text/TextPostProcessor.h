#pragma once

#include <QList>
#include <QPair>
#include <QString>

// Постобработка текста, который выдаёт ASR.
//
// Зачем: ни zipformer-ru, ни GigaAM (кроме недоступных в ONNX e2e-версий) не ставят
// знаки препинания и заглавные буквы. Русской модели пунктуации в sherpa-onnx нет.
// Поэтому:
//   1) голосовые знаки препинания ("точка" -> ".", "запятая" -> ",") — надёжный способ;
//   2) граница сегмента VAD = граница предложения: заглавная в начале, точка в конце;
//   3) косметика: убрать пробел перед знаком, схлопнуть повторы.
//
// Класс намеренно НЕ QObject: чистая функция над строкой, удобно тестировать.
class TextPostProcessor {
public:
    struct Options {
        bool voicePunctuation  = true;   // "точка" -> "."
        bool capitalizeSentences = true; // заглавная после начала текста и после ". "
        bool addFinalDot       = true;   // точка в конце, если знака нет
        bool collapseSpaces    = true;   // лишние пробелы и пробел перед знаком
    };

    using Rule = QPair<QString, QString>;   // фраза (строчными) -> что вставить

    // Два конструктора вместо `= Options()` в аргументе: GCC не позволяет
    // использовать default member initializers вложенного структура в default
    // argument того же класса ("required before the end of its enclosing class").
    TextPostProcessor();
    explicit TextPostProcessor(const Options& options);

    QString process(const QString& raw) const;

    void setRules(const QList<Rule>& rules);
    const QList<Rule>& rules() const { return m_rules; }

    // Фразы-знаки ("точка", "вопросительный знак", ...) — их стоит добавлять
    // в hotwords, иначе ASR их проглатывает: модель обучена на живой речи и не
    // ожидает диктантных команд. Проверено замером: с ними в hotwords текст
    // "…помощника точка набор текста работает запятая…" распознаётся целиком,
    // без них оба "точка" теряются. Слово "пробел" не возвращаем: его буст
    // рискованнее, чем полезен.
    QStringList punctuationPhrases() const;

    // Порядок важен: сначала самые длинные фразы, иначе "точка с запятой"
    // превратится в "точка" + хвост.
    static QList<Rule> defaultRules();

    // Нужен ли пробел перед очередным сегментом при вставке.
    //
    // VAD режет речь на фразы, и каждая вставляется отдельным вызовом typeText().
    // Без разделителя получается "…двадцать лет назад.Сегодня вот…" — именно так
    // это и выглядело в первом живом прогоне.
    //
    // Правила: пробел нужен, если предыдущий сегмент не кончился пробелом/переводом
    // строки, а следующий не начинается со знака препинания или закрывающей кавычки.
    static bool needsLeadingSpace(const QString& previous, const QString& next);

private:
    QString applyVoicePunctuation(const QString& text) const;
    static QString fixSpacing(const QString& text);
    static QString capitalizeSentences(const QString& text);
    static QString ensureFinalDot(const QString& text);

    Options m_options;
    QList<Rule> m_rules;   // отсортированы по убыванию числа слов
};
