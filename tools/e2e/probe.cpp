// tools/e2e/probe.cpp — тестовое окно для e2e-проверок вывода.
//
// QLineEdit + перехват сырых xcb-событий: считаем, сколько нажатий пришло
// с флагом send_event=1 (синтетика XSendEvent, то есть xdotool --window)
// и сколько настоящими событиями (XTest / обычная печать). Каждые 200 мс
// состояние пишется в файл (--dump), чтобы сценарий мог прочитать его в любой
// момент.
//
// Намеренно НЕ включает <xcb/xcb.h> и не линкует libxcb: из xcb-события нужен
// ровно первый байт (response_type), а его структура стабильна с 1987 года.
// Так пробник собирается везде, где есть Qt5 Widgets, без libxcb1-dev.
//
// Запуск:
//   ./probe <appName> <заголовок> <файл-дампа> [время жизни, мс]
// appName задаёт WM_CLASS окна (нужно для проверки own_window_class:
// Qt пишет applicationName в поле res_class).

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

namespace {

// Первые байты любого X-события: response_type; бит 0x80 в нём = send_event
// (событие synthetic, отправлено через XSendEvent). KeyPress = 2.
constexpr uint8_t kKeyPressCode = 2;
constexpr uint8_t kSendEventBit = 0x80;

int g_keyPressReal = 0;      // send_event = 0
int g_keyPressSynthetic = 0; // send_event = 1

class NativeFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message, long* /*result*/) override
    {
        if (eventType != QByteArrayLiteral("xcb_generic_event_t")) {
            return false;
        }
        const uint8_t responseType = *static_cast<const uint8_t*>(message);
        if ((responseType & 0x7f) == kKeyPressCode) {
            if (responseType & kSendEventBit) {
                ++g_keyPressSynthetic;
            } else {
                ++g_keyPressReal;
            }
        }
        return false;
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        qCritical("usage: probe <appName> <title> <dumpfile> [lifetimeMs]");
        return 2;
    }
    const QString appName  = QString::fromLocal8Bit(argv[1]);
    const QString title    = QString::fromLocal8Bit(argv[2]);
    const QString dumpPath = QString::fromLocal8Bit(argv[3]);
    const int lifetimeMs   = (argc > 4) ? atoi(argv[4]) : 120000;

    QApplication app(argc, argv);
    app.setApplicationName(appName);   // -> res_class в WM_CLASS окна

    NativeFilter filter;
    app.installNativeEventFilter(&filter);

    QWidget w;
    w.setWindowTitle(title);
    auto* edit = new QLineEdit(&w);
    edit->setGeometry(10, 10, 460, 32);
    w.setGeometry(60, 60, 480, 60);
    w.show();
    edit->setFocus();

    QTimer dump;
    QObject::connect(&dump, &QTimer::timeout, [&]() {
        QFile f(dumpPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&f);
            out.setCodec("UTF-8");
            out << "text=" << edit->text() << "\n"
                << "real=" << g_keyPressReal << "\n"
                << "synthetic=" << g_keyPressSynthetic << "\n";
        }
    });
    dump.start(200);

    QTimer::singleShot(lifetimeMs, &app, &QCoreApplication::quit);
    return app.exec();
}
