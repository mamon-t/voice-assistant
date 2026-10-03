#pragma once

#include <QtGlobal>

// Глушитель микрофона на время печати.
//
// Задача. Если диктовать и одновременно печатать, возникают две беды:
//   1. микрофон ловит стук клавиш, VAD делает из него сегмент, и в текст
//      вставляется мусор;
//   2. продиктованное вклинивается в напечатанное с опозданием на 0.5–2 с
//      (столько живёт путь VAD -> ASR -> постобработка).
//
// Решение. evdev читается и так (глобальный хоткей), поэтому момент нажатия
// любой клавиши известен. Пока с последнего нажатия прошло меньше guardMs,
// кусок аудио не уходит в VAD/ASR.
//
// Важно: запись в WAV (проверка микрофона) НЕ прерывается — диагностика
// обязана видеть весь тракт целиком, иначе снимок микрофона перестаёт
// соответствовать тому, что слышит ASR, и сравнение «файл против эфира»
// теряет смысл.
//
// Класс намеренно не знает ни про evdev, ни про Qt-события, ни про аудио:
// время передаётся снаружи, поэтому логика проверяется тестами.
class TypingGuard {
public:
    explicit TypingGuard(int guardMs = 0)
        : m_guardMs(guardMs < 0 ? 0 : guardMs)
    {
    }

    void setGuardMs(int ms) { m_guardMs = (ms < 0) ? 0 : ms; }
    int  guardMs() const { return m_guardMs; }
    bool isEnabled() const { return m_guardMs > 0; }

    // Любое нажатие/отпускание/автоповтор клавиши, КРОМЕ самой клавиши хоткея
    void noteKeyActivity(qint64 nowMs)
    {
        m_lastKeyMs = nowMs;
        m_haveKey   = true;
    }

    // true = этот кусок аудио надо выбросить, не доводя до VAD
    bool blocked(qint64 nowMs) const
    {
        if (m_guardMs <= 0 || !m_haveKey) {
            return false;
        }
        return (nowMs - m_lastKeyMs) < m_guardMs;
    }

    void reset()
    {
        m_haveKey       = false;
        m_droppedChunks  = 0;
        m_droppedSamples = 0;
    }

    // Счётчики — для строки в логе по окончании записи: сколько аудио
    // выброшено и за какое время. Без них непонятно, не съедает ли глушитель
    // собственно речь.
    void   countDropped(qint64 samples) { ++m_droppedChunks; m_droppedSamples += samples; }
    qint64 droppedChunks()  const { return m_droppedChunks; }
    qint64 droppedSamples() const { return m_droppedSamples; }

private:
    int    m_guardMs   = 0;
    qint64 m_lastKeyMs = 0;
    bool   m_haveKey   = false;

    qint64 m_droppedChunks  = 0;
    qint64 m_droppedSamples = 0;
};
