// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// The shell-side CPU sampler. `linux/platform/sensors` reads /proc/stat and
// this pushes each reading into the core's `metrics` service as the
// `{"sample":<percent>}` command BRIDGE.md documents, so the panel's
// sparkline renders a real measurement rather than a generated series.
//
// Why the shell samples: `MetricsBridgeService` deliberately owns the ring
// buffer and nothing else (`Sources/VorssaintCore/Bridge/MetricsBridge.swift`),
// because WP-A1 is what wires `SystemSensors` into the core. Until it does,
// the shell drives the cadence, which is what `MetricsCommand.sample` exists
// for. When it lands, this class deletes and nothing else changes.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class MetricsSampler : public QObject {
    Q_OBJECT

public:
    explicit MetricsSampler(QObject *parent = nullptr);
    ~MetricsSampler() override;

    // Start sampling at `intervalMs`. False when there is no sensor backend;
    // `description()` then says why, and the panel says so to the user.
    bool start(int intervalMs = 500);

    // One line naming where the numbers come from, rendered in the panel.
    // Never optimistic: it says "placeholder" whenever it is not measuring.
    QString description() const { return m_description; }
    bool isReal() const { return m_real; }

    // Take one reading and push it to the bridge. Returns the percentage
    // pushed, or -1 when there was nothing to push (no backend, or the first
    // reading of an instance, which has no interval to divide by).
    double sampleOnce();

private:
    QTimer m_timer;
    QString m_description = QStringLiteral("placeholder data (no sensor backend)");
    bool m_real = false;
    void *m_sensors = nullptr;
};
