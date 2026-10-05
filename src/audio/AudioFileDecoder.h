#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

// Декодирование аудиоФАЙЛА в формат, который понимает боевой конвейер:
// моно, 16 кГц, int16 (ровно то же требование, что у SileroVad в sherpa-onnx).
//
// Зачем отдельный класс. Прогон записи через VAD+ASR уже был в tools/vad_asr_test,
// но свой WAV-ридер жил прямо в инструменте. Фича «разбери аудиофайл» нужна
// и в GUI (команда «разбери файл» / пункт меню лотка), и в консоли
// (--transcribe), поэтому чтение файла стало общим модулем ядра:
//
//   * WAV        — свой парсер: PCM 8/16/24/32 бит, float32, A-law, μ-law,
//                  любое число каналов и любая частота;
//   * RAW        — PCM без контейнера (.raw/.pcm/.audio и любые неизвестные
//                  расширения): параметры (частота/каналы/кодирование) некуда
//                  записать, их задаёт вызывающий — RawParams. Телефонные
//                  записи обычно 8000 Гц моно, 16 бит или G.711;
//   * остальное  — mp3/ogg/flac/m4a/amr… через QAudioDecoder (QtMultimedia,
//                  в Linux это GStreamer): поддержка конкретного формата
//                  зависит от установленных системных плагинов.
//
// Все пути приводят результат к моно 16 кГц (домикширование + линейная
// передискретизация — тот же алгоритм, что был в vad_asr_test).
class AudioFileDecoder {
public:
    // Кодирование сэмплов для RAW-файлов (без контейнера).
    enum class RawFormat {
        S16LE,   // 16 бит знаковое LE — самый частый случай
        S8U,     // 8 бит без знака (центр 128) — старые WAV/RAW
        F32LE,   // 32 бит float LE
        ALAW,    // G.711 A-law — телефония (Европа/РФ)
        ULAW     // G.711 μ-law — телефония (США/Япония)
    };

    struct RawParams {
        int       sampleRate = 8000;              // телефонный стандарт
        int       channels   = 1;
        RawFormat format     = RawFormat::S16LE;

        // "s16le" | "s8u" | "f32le" | "alaw" | "ulaw"; неизвестное -> S16LE.
        static RawFormat  formatFromString(const QString& s);
        static QString    formatToString(RawFormat f);
        static QStringList formatNames();          // все имена — для CLI-подсказки и combos
        int bytesPerSample() const;
    };

    struct Audio {
        std::vector<int16_t> samples;   // моно, 16 кГц — можно кормить в VoicePipeline
        int     sourceRate     = 0;     // исходная частота (для справки/логов)
        int     sourceChannels = 0;
        QString sourceFormat;           // человекочитаемо: "PCM16", "RAW s16le", "GStreamer"
        double  seconds() const { return double(samples.size()) / 16000.0; }
    };

    // Фильтр для QFileDialog и подсказок CLI.
    static QString fileDialogFilter();

    // Расширение говорит «это RAW без контейнера» (.raw/.pcm/.audio).
    // Окончательное решение принимает decode(): неизвестное расширение
    // без контейнерного magic тоже уходит в RAW-ветку.
    static bool isRawExtension(const QString& path);

    // Основной вход. Маршрутизация: RIFF/WAVE -> парсер WAV; известное
    // «сжатое» расширение -> QAudioDecoder; всё остальное -> RAW с rawParams.
    // Битый/экзотический WAV дополнительно пробуется через QAudioDecoder.
    static bool decode(const QString& path, const RawParams& rawParams,
                       Audio& out, QString* error);

    // G.711 наружу — для модульных тестов: alaw-тишина 0xD5 -> +8,
    // ulaw-тишина 0xFF -> 0 (классические контрольные значения).
    static int16_t alawToLinear(uint8_t v);
    static int16_t ulawToLinear(uint8_t v);

    // Линейная передискретизация моно-буфера в 16 кГц (in-place).
    // srcRate == 16000 -> no-op. Вынесена для тестов.
    static void resampleTo16k(std::vector<int16_t>& mono, int srcRate);

private:
    static bool decodeWav(const QByteArray& data, Audio& out, QString* error);
    static bool decodeRaw(const QByteArray& data, const RawParams& p,
                          Audio& out, QString* error);
    // QAudioDecoder + локальный QEventLoop: работает и в главном потоке (CLI),
    // и в рабочем потоке FileTranscriber. Запрос s16/16к/моно — GStreamer сам
    // вставляет audioconvert/audioresample, но на случай, если бэкенд проигнорировал
    // запрос, буферы конвертируются вручную по фактическому формату.
    static bool decodeViaQt(const QString& path, Audio& out, QString* error);
};
