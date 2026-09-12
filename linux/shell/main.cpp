// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// The Linux app shell (WP-20 slice). One binary: a StatusNotifierItem tray
// item, and the three Qt Quick screens WP-01 measured, selected with
// --screen panel|prefs|overlay.
//
//   --version     print the version and exit, so a package can be smoke-
//                 tested with no display at all
//   --selftest    run the headless checks and exit non-zero on the first
//                 failure: the bridge answers for all three services, the
//                 sampler reaches /proc/stat, and every screen's QML loads
//   --screen S    panel (default), prefs, overlay
//   --demo-drag   drive the overlay's drag rectangle, for headless shots
//   --quit-after N exit after N ms (the RSS measurement and CI smoke)
#include "CoreModel.h"
#include "MetricsSampler.h"

#include <QApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QMenu>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>

extern "C" {
#include "corebridge.h"
}

namespace {

// Until PLAN.md section 10.1 is settled this identity is deliberately the
// port's own, not the macOS product's: TRADEMARKS.md reserves the official
// name, icon and bundle id for builds the maintainer distributes. WP-41
// renames both this and the .desktop file in one change if the owner picks a
// different identity.
const char *kAppID = "com.vorssaint.VorssaintLinux";
const char *kAppName = "Vorssaint";
const char *kVersion = "0.1.0-dev";

int selftest(MetricsSampler &sampler, QQmlApplicationEngine &engine)
{
    int checks = 0;
    int failures = 0;
    const auto check = [&](const char *what, bool ok, const QString &detail) {
        checks++;
        if (!ok)
            failures++;
        std::printf("[%s] %s%s%s\n", ok ? " OK " : "FAIL", what,
                    detail.isEmpty() ? "" : ": ",
                    detail.isEmpty() ? "" : qPrintable(detail));
    };

    // 1. The bridge answers for every service the shell binds, and what it
    //    answers is a JSON object. A stub and the real core both pass this;
    //    what it catches is an archive that linked but registered nothing.
    for (const char *service : { "metrics", "featureRuntime", "l10n" }) {
        char *json = vs_snapshot(service);
        const QString text = json ? QString::fromUtf8(json) : QString();
        if (json)
            vs_free(json);
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
        check(service, doc.isObject(),
              text.isEmpty() ? QStringLiteral("no snapshot") : text.left(120));
    }

    // 2. The sampler. A machine with no readable /proc/stat is a real
    //    failure on Linux, not a tolerated one.
    check("sampler", sampler.isReal(), sampler.description());
    // The first reading of an instance has no previous one to subtract, and a
    // second taken inside the same jiffy has nothing to divide by either, so
    // the check waits for a real interval rather than asserting on a race.
    double percent = -1;
    for (int attempt = 0; attempt < 20 && percent < 0; attempt++) {
        QThread::msleep(100);
        percent = sampler.sampleOnce();
    }
    check("cpu sample", percent >= 0.0 && percent <= 100.0,
          QStringLiteral("%1 %").arg(percent, 0, 'f', 2));

    // What the panel binds: a sample went in and history came back out of the
    // same snapshot the QML reads, so the shell and the core are wired to each
    // other and not merely both alive.
    if (char *json = vs_snapshot("metrics")) {
        const QJsonObject state = QJsonDocument::fromJson(QByteArray(json)).object();
        vs_free(json);
        const QJsonArray history = state.value("history").toArray();
        check("metrics round trip", !history.isEmpty(),
              QStringLiteral("cpu=%1 history=%2")
                  .arg(state.value("cpu").toDouble(), 0, 'f', 2)
                  .arg(history.size()));
    } else {
        check("metrics round trip", false, QStringLiteral("no snapshot"));
    }

    // 3. Every screen's QML compiles and instantiates. This is what a
    //    missing QML module in a package looks like, and it is the whole
    //    reason the flag exists: it fails in a container with no display.
    for (const char *screen : { "Panel", "Preferences", "Overlay" }) {
        engine.clearComponentCache();
        engine.load(QUrl(QStringLiteral("qrc:/qml/%1.qml").arg(screen)));
        const bool loaded = !engine.rootObjects().isEmpty()
            && engine.rootObjects().last() != nullptr;
        check(screen, loaded, QString());
    }

    std::printf("SELFTEST %s (%d checks, %d failed)\n",
                failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QString screen = QStringLiteral("panel");
    bool demoDrag = false;
    bool wantSelftest = false;
    int quitAfter = 0;
    for (int i = 1; i < argc; i++) {
        const QString a = QString::fromUtf8(argv[i]);
        if (a == "--version") {
            std::printf("%s %s (linux)\n", kAppName, kVersion);
            return 0;
        }
        if (a == "--selftest") wantSelftest = true;
        else if (a == "--screen" && i + 1 < argc) screen = QString::fromUtf8(argv[++i]);
        else if (a == "--demo-drag") demoDrag = true;
        else if (a == "--quit-after" && i + 1 < argc) quitAfter = QString::fromUtf8(argv[++i]).toInt();
        else if (a == "--help" || a == "-h") {
            std::printf("usage: vorssaint [--version] [--selftest] "
                        "[--screen panel|prefs|overlay] [--quit-after ms]\n");
            return 0;
        }
    }

    // --selftest must run where there is no display at all, and must not
    // silently pick up a session's compositor when there is one.
    if (wantSelftest)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8(kAppName));
    app.setApplicationVersion(QString::fromUtf8(kVersion));
    app.setDesktopFileName(QString::fromUtf8(kAppID));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/vorssaint.png")));

    qmlRegisterType<CoreModel>("Vorssaint", 1, 0, "CoreModel");

    MetricsSampler sampler;
    bool sampling = sampler.start(500);
    QString metricsSource = sampler.description();
#ifdef VORSSAINT_STUB_BRIDGE
    // Built against the WP-01 C stub: whatever the sampler reads, what the
    // panel draws is the stub's generated series, so the panel says that and
    // not what the sampler would like to claim.
    sampling = false;
    metricsSource = QStringLiteral("WP-01 stub bridge, generated series");
#endif
    std::fprintf(stderr, "metrics: %s\n", qPrintable(metricsSource));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("screenName", screen);
    engine.rootContext()->setContextProperty("demoDrag", demoDrag);
    engine.rootContext()->setContextProperty("metricsSource", metricsSource);
    engine.rootContext()->setContextProperty("metricsIsReal", sampling);
    engine.rootContext()->setContextProperty("appVersion", QString::fromUtf8(kVersion));

    if (wantSelftest)
        return selftest(sampler, engine);

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
    tray.setIcon(QIcon(QStringLiteral(":/icons/vorssaint.png")));
    tray.setToolTip(QStringLiteral("%1 %2").arg(QString::fromUtf8(kAppName),
                                                QString::fromUtf8(kVersion)));
    tray.show();
    std::fprintf(stderr, "tray: visible=%s\n", tray.isVisible() ? "true" : "false");

    const QString file = screen == "prefs" ? "Preferences"
                       : screen == "overlay" ? "Overlay" : "Panel";
    engine.load(QUrl("qrc:/qml/" + file + ".qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    if (quitAfter > 0)
        QTimer::singleShot(quitAfter, &app, &QApplication::quit);

    return app.exec();
}
