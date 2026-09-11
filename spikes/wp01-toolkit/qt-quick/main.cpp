// WP-01 Qt 6 Quick candidate. One binary, three screens, selected with
// --screen panel|prefs|overlay so each can be screenshotted on its own.
//   --demo-drag  drives the overlay's drag-rectangle programmatically, for
//                headless screenshots where there is no pointer device.
//   --quit-after <ms>  exits after N ms (used by the RSS measurement).
#include "CoreModel.h"

#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QMenu>
#include <QPixmap>
#include <QPainter>
#include <QtGlobal>

#include <cstdio>

static QIcon trayIcon()
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x4a, 0x90, 0xd9));
    p.drawRoundedRect(2, 2, 18, 18, 5, 5);
    p.setPen(QPen(Qt::white, 2));
    p.drawPolyline(QPolygon({ { 6, 15 }, { 9, 9 }, { 12, 13 }, { 16, 6 } }));
    return QIcon(pm);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("vorssaint-qt-spike");

    QString screen = "panel";
    bool demoDrag = false;
    int quitAfter = 0;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromUtf8(argv[i]);
        if (a == "--screen" && i + 1 < argc) screen = QString::fromUtf8(argv[++i]);
        else if (a == "--demo-drag") demoDrag = true;
        else if (a == "--quit-after" && i + 1 < argc) quitAfter = QString::fromUtf8(argv[++i]).toInt();
    }

    qmlRegisterType<CoreModel>("Vorssaint", 1, 0, "CoreModel");

    // The tray entry point. On a session with a StatusNotifierWatcher, Qt's
    // XDG platform theme registers this as a StatusNotifierItem over D-Bus;
    // with no watcher it falls back to an X11 XEmbed icon, and reports
    // unavailable when there is neither.
    QSystemTrayIcon tray;
    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    std::fprintf(stderr, "tray: isSystemTrayAvailable=%s\n", trayAvailable ? "true" : "false");
    QMenu trayMenu;
    trayMenu.addAction("Panel");
    trayMenu.addAction("Preferences");
    trayMenu.addAction("Quit", &app, &QApplication::quit);
    tray.setContextMenu(&trayMenu);
    tray.setIcon(trayIcon());
    tray.setToolTip("Vorssaint (WP-01 Qt spike)");
    tray.show();
    std::fprintf(stderr, "tray: visible=%s\n", tray.isVisible() ? "true" : "false");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("screenName", screen);
    engine.rootContext()->setContextProperty("demoDrag", demoDrag);
    const QString file = screen == "prefs" ? "Preferences" : screen == "overlay" ? "Overlay" : "Panel";
    engine.load(QUrl("qrc:/qml/" + file + ".qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (quitAfter > 0)
        QTimer::singleShot(quitAfter, &app, &QApplication::quit);

    return app.exec();
}
