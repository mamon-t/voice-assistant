#include "input/EvdevHotkeyListener.h"

#include <QDebug>
#include <QDir>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

struct KeyName {
    const char* name;
    int         code;
};

// Небольшой набор имён, которого хватает для хоткея микрофона.
// Полный список — в /usr/include/linux/input-event-codes.h; можно также
// задать код числом: key=66 (это KEY_F8).
const KeyName kKeyNames[] = {
    { "f1", KEY_F1 },   { "f2", KEY_F2 },   { "f3", KEY_F3 },   { "f4", KEY_F4 },
    { "f5", KEY_F5 },   { "f6", KEY_F6 },   { "f7", KEY_F7 },   { "f8", KEY_F8 },
    { "f9", KEY_F9 },   { "f10", KEY_F10 }, { "f11", KEY_F11 }, { "f12", KEY_F12 },
    { "f13", KEY_F13 }, { "f14", KEY_F14 }, { "f15", KEY_F15 }, { "f16", KEY_F16 },
    { "space", KEY_SPACE },     { "enter", KEY_ENTER },   { "return", KEY_ENTER },
    { "tab", KEY_TAB },         { "esc", KEY_ESC },       { "escape", KEY_ESC },
    { "backspace", KEY_BACKSPACE },
    { "insert", KEY_INSERT },   { "delete", KEY_DELETE },
    { "home", KEY_HOME },       { "end", KEY_END },
    { "pageup", KEY_PAGEUP },   { "pagedown", KEY_PAGEDOWN },
    { "up", KEY_UP },           { "down", KEY_DOWN },     { "left", KEY_LEFT },
    { "right", KEY_RIGHT },
    { "scrolllock", KEY_SCROLLLOCK }, { "pause", KEY_PAUSE },
    { "micmute", KEY_MICMUTE }, { "f20", KEY_F20 },
};

const KeyName kModifierNames[] = {
    { "ctrl",  KEY_LEFTCTRL },
    { "control", KEY_LEFTCTRL },
    { "alt",   KEY_LEFTALT },
    { "shift", KEY_LEFTSHIFT },
    { "super", KEY_LEFTMETA },
    { "meta",  KEY_LEFTMETA },
    { "win",   KEY_LEFTMETA },
};

}  // namespace

EvdevHotkeyListener::EvdevHotkeyListener(QObject* parent)
    : IHotkeyListener(parent)
{
}

EvdevHotkeyListener::EvdevHotkeyListener(const Options& options, QObject* parent)
    : IHotkeyListener(parent)
    , m_options(options)
{
}

EvdevHotkeyListener::~EvdevHotkeyListener()
{
    stop();
}

void EvdevHotkeyListener::setOptions(const Options& options)
{
    m_options = options;
}

int EvdevHotkeyListener::keyNameToCode(const QString& name)
{
    const QString n = name.trimmed().toLower().remove(QStringLiteral("key_"));
    if (n.isEmpty()) {
        return -1;
    }
    for (const KeyName& k : kKeyNames) {
        if (n == QLatin1String(k.name)) {
            return k.code;
        }
    }
    bool ok = false;
    const int asNumber = n.toInt(&ok);
    return ok ? asNumber : -1;
}

quint32 EvdevHotkeyListener::modifierBitForKey(int code)
{
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:  return ModCtrl;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_ALTERASE:   return ModAlt;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: return ModShift;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:  return ModMeta;
    default:             return ModNone;
    }
}

bool EvdevHotkeyListener::parseKey(const QString& spec, int* keyCode,
                                   quint32* modifiers, QString* error)
{
    const auto fail = [error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        return false;
    };

    const QString trimmed = spec.trimmed();
    if (trimmed.isEmpty()) {
        return fail(QStringLiteral("пустая спецификация клавиши"));
    }

    const QStringList parts = trimmed.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return fail(QStringLiteral("пустая спецификация клавиши"));
    }

    quint32 mods = ModNone;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        const QString m = parts.at(i).trimmed().toLower();
        bool found = false;
        for (const KeyName& kn : kModifierNames) {
            if (m == QLatin1String(kn.name)) {
                mods |= modifierBitForKey(kn.code);
                found = true;
                break;
            }
        }
        if (!found) {
            return fail(QStringLiteral("неизвестный модификатор '%1' (доступны: ctrl, alt, shift, super)")
                            .arg(parts.at(i)));
        }
    }

    const int code = keyNameToCode(parts.last());
    if (code < 0) {
        return fail(QStringLiteral("неизвестная клавиша '%1'. Примеры: F8, KEY_F13, ctrl+space, "
                                   "или числовой код (66 = KEY_F8)")
                        .arg(parts.last()));
    }

    // Комбинация «модификатор + та же клавиша» смысла не имеет
    if (modifierBitForKey(code) != ModNone && parts.size() > 1) {
        return fail(QStringLiteral("'%1' — модификатор, его нельзя одновременно "
                                   "использовать как основную клавишу в комбинации")
                        .arg(parts.last()));
    }
    if (modifierBitForKey(code) != ModNone) {
        // одиночный модификатор как хоткей (например, просто Ctrl) — допустимо
        mods |= modifierBitForKey(code);
    }

    if (keyCode) {
        *keyCode = code;
    }
    if (modifiers) {
        *modifiers = mods;
    }
    return true;
}

bool EvdevHotkeyListener::handleEvent(const struct input_event& ev)
{
    if (ev.type != EV_KEY) {
        return false;
    }

    // Модификаторы отслеживаем всегда, даже если это не наша клавиша
    if (const quint32 bit = modifierBitForKey(ev.code); bit != ModNone) {
        if (ev.value == 0) {
            m_activeMods &= ~bit;
        } else {
            m_activeMods |= bit;      // value 1 (нажатие) и 2 (автоповтор)
        }
        return false;
    }

    if (ev.code != m_keyCode) {
        return false;
    }

    // value: 0 = отпущена, 1 = нажата, 2 = автоповтор (игнорируем,
    // иначе push-to-talk дёргался бы непрерывно)
    if (ev.value == 1 && !m_down) {
        const quint32 required = m_modifiers & ~modifierBitForKey(m_keyCode);
        if ((m_activeMods & required) != required) {
            return false;             // нажата не та комбинация
        }
        m_down = true;
        emit pressed();
        return true;
    }

    if (ev.value == 0 && m_down) {
        m_down = false;
        emit released();
        return true;
    }

    return false;
}

bool EvdevHotkeyListener::openDevices(QString* error)
{
    QDir dir(QStringLiteral("/dev/input"));
    if (!dir.exists()) {
        if (error) {
            *error = QStringLiteral("/dev/input не существует — evdev недоступен");
        }
        return false;
    }

    const QStringList entries = dir.entryList({QStringLiteral("event*")}, QDir::System | QDir::Files,
                                              QDir::Name);
    if (entries.isEmpty()) {
        if (error) {
            *error = QStringLiteral("в /dev/input нет устройств event*");
        }
        return false;
    }

    int opened = 0;
    int denied  = 0;

    for (const QString& name : entries) {
        const QString path = dir.filePath(name);
        const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM) {
                ++denied;
            }
            continue;
        }

        // Нужны только устройства с клавишами
        unsigned char keyBits[KEY_MAX / 8 + 1];
        std::memset(keyBits, 0, sizeof(keyBits));
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) {
            ::close(fd);
            continue;
        }
        const bool hasKeys = (keyBits[m_keyCode / 8] & (1u << (m_keyCode % 8))) != 0;
        if (!hasKeys) {
            ::close(fd);
            continue;
        }

        char devName[256] = {0};
        if (::ioctl(fd, EVIOCGNAME(sizeof(devName) - 1), devName) < 0) {
            std::strncpy(devName, "unknown", sizeof(devName) - 1);
        }
        m_deviceNames << QStringLiteral("%1 (%2)").arg(path, QString::fromLocal8Bit(devName));

        if (m_options.grab) {
            int grab = 1;
            if (::ioctl(fd, EVIOCGRAB, &grab) == 0) {
                m_grabbedFds << fd;
                qWarning().noquote()
                    << QStringLiteral("EvdevHotkey: устройство %1 ПЕРЕХВАЧЕНО эксклюзивно "
                                      "(EVIOCGRAB). Остальные приложения, включая X-сервер, "
                                      "перестанут получать с него ВСЕ клавиши. Это имеет смысл "
                                      "только для выделенной педали или второй клавиатуры.")
                           .arg(path);
            } else {
                qWarning().noquote()
                    << QStringLiteral("EvdevHotkey: не смог перехватить %1 (%2), "
                                      "работаю в режиме наблюдения")
                           .arg(path, QString::fromLocal8Bit(std::strerror(errno)));
            }
        }

        auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this, fd]() { onDeviceReadable(fd); });

        m_fds << fd;
        m_notifiers << notifier;
        ++opened;
    }

    if (opened == 0) {
        if (error) {
            *error = QStringLiteral(
                "Не удалось открыть ни одного устройства с клавишами в /dev/input "
                "(доступ запрещён для %1 из %2). Нужно: sudo usermod -aG input $USER, "
                "затем перелогин. Либо повесьте клавишу в настройках WM на D-Bus-вызов "
                "startRecording/stopRecording.").arg(denied).arg(entries.size());
        }
        return false;
    }
    return true;
}

void EvdevHotkeyListener::onDeviceReadable(int fd)
{
    struct input_event ev;
    // Читаем всё, что накопилось: при активном наборе событий может быть много
    for (;;) {
        const ssize_t n = ::read(fd, &ev, sizeof(ev));
        if (n != static_cast<ssize_t>(sizeof(ev))) {
            break;   // EAGAIN, конец или частичное событие — ждём следующей активации
        }
        handleEvent(ev);
    }
}

bool EvdevHotkeyListener::configure(QString* error)
{
    QString parseError;
    if (!parseKey(m_options.key, &m_keyCode, &m_modifiers, &parseError)) {
        const QString msg = QStringLiteral("EvdevHotkey: %1 (key='%2')")
                                .arg(parseError, m_options.key);
        if (error) {
            *error = msg;
        }
        return false;
    }
    return true;
}

bool EvdevHotkeyListener::start(QString* error)
{
    if (isActive()) {
        return true;
    }

    QString parseError;
    if (!configure(&parseError)) {
        if (error) {
            *error = parseError;
        }
        emit errorOccurred(parseError);
        return false;
    }

    QString openError;
    QString* err = error ? error : &openError;   // error может быть nullptr — не разыменовываем его
    if (!openDevices(err)) {
        if (!err->isEmpty()) {
            emit errorOccurred(*err);
        }
        stop();
        return false;
    }

    qInfo().noquote()
        << QStringLiteral("EvdevHotkey: клавиша '%1' (code=%2, модификаторы=0x%3), устройств: %4 — %5")
               .arg(m_options.key)
               .arg(m_keyCode)
               .arg(m_modifiers, 0, 16)
               .arg(m_fds.size())
               .arg(m_deviceNames.join(QStringLiteral(", ")));
    return true;
}

void EvdevHotkeyListener::stop()
{
    for (int fd : std::as_const(m_grabbedFds)) {
        int grab = 0;
        ::ioctl(fd, EVIOCGRAB, &grab);   // отпускаем устройство
    }
    m_grabbedFds.clear();

    for (QSocketNotifier* n : std::as_const(m_notifiers)) {
        n->setEnabled(false);
        n->deleteLater();
    }
    m_notifiers.clear();

    for (int fd : std::as_const(m_fds)) {
        ::close(fd);
    }
    m_fds.clear();
    m_deviceNames.clear();
    m_activeMods = ModNone;
    m_down = false;
}

bool EvdevHotkeyListener::isActive() const
{
    return !m_fds.isEmpty();
}

QString EvdevHotkeyListener::backendName() const
{
    return QStringLiteral("evdev (%1)")
        .arg(isActive() ? QStringLiteral("%1 устр.").arg(m_fds.size())
                        : QStringLiteral("не запущен"));
}
