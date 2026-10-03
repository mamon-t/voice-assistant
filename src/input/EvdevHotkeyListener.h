#pragma once

#include "input/IHotkeyListener.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <linux/input.h>

class QSocketNotifier;

// Глобальный хоткей через evdev: читаем /dev/input/event* напрямую, поэтому
// работает и в X11, и в Wayland, и в голой TTY — системе эта клавиша не нужна.
//
// Требует прав на чтение /dev/input/event*:
//   sudo usermod -aG input $USER     (и перелогин)
// Если прав нет, start() вернёт false с внятным сообщением, а приложение
// продолжит работать — переключать режим можно из трея и по D-Bus.
//
// Логика разбора событий вынесена в handleEvent() и не зависит от файловых
// дескрипторов, поэтому покрыта тестами на синтетических input_event
// (железо для тестов не нужно).
class EvdevHotkeyListener : public IHotkeyListener {
    Q_OBJECT

public:
    // Логические модификаторы: левый и правый Ctrl считаются одним и тем же
    enum Modifier : quint32 {
        ModNone  = 0,
        ModCtrl  = 1u << 0,
        ModAlt   = 1u << 1,
        ModShift = 1u << 2,
        ModMeta  = 1u << 3,
    };

    struct Options {
        QString key = QStringLiteral("KEY_F8");   // "F8", "KEY_F8", "ctrl+space", "66"
        bool    grab = true;                      // EVIOCGRAB: не отдавать клавишу системе
    };

    explicit EvdevHotkeyListener(QObject* parent = nullptr);
    EvdevHotkeyListener(const Options& options, QObject* parent = nullptr);
    ~EvdevHotkeyListener() override;

    bool start(QString* error = nullptr) override;
    void stop() override;
    bool isActive() const override;
    QString backendName() const override;

    void setOptions(const Options& options);
    Options options() const { return m_options; }

    QStringList devices() const { return m_deviceNames; }

    // --- тестируемая часть ---

    // Разбирает и валидирует опции БЕЗ открытия /dev/input.
    // Нужен для тестов и для диагностики (--check): права на evdev не требуются.
    bool configure(QString* error = nullptr);

    // "ctrl+space" / "KEY_F8" / "F8" / "66"  ->  код клавиши + маска модификаторов
    static bool parseKey(const QString& spec, int* keyCode, quint32* modifiers, QString* error);

    // Один evdev-событийный шаг. Возвращает true, если событие породило
    // pressed()/released(). Обновляет состояние модификаторов.
    bool handleEvent(const struct input_event& ev);

    static quint32 modifierBitForKey(int code);
    static int keyNameToCode(const QString& name);   // -1, если имя неизвестно

private:
    void onDeviceReadable(int fd);
    bool openDevices(QString* error);

    Options m_options;

    int      m_keyCode     = -1;
    quint32  m_modifiers   = ModNone;
    quint32  m_activeMods  = ModNone;
    bool     m_down        = false;

    QVector<int>              m_fds;
    QVector<QSocketNotifier*> m_notifiers;
    QVector<int>              m_grabbedFds;
    QStringList               m_deviceNames;
};
