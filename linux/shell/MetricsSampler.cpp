// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
#include "MetricsSampler.h"

#include <QByteArray>

extern "C" {
#include "corebridge.h"
}

#ifdef VORSSAINT_HAVE_SENSORS
extern "C" {
#include "vorssaint_platform.h"
}
#endif

MetricsSampler::MetricsSampler(QObject *parent) : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, [this] { sampleOnce(); });
}

MetricsSampler::~MetricsSampler()
{
#ifdef VORSSAINT_HAVE_SENSORS
    if (m_sensors) {
        auto *sys = static_cast<vs_sensors_system *>(m_sensors);
        sys->destroy(sys);
        m_sensors = nullptr;
    }
#endif
}

bool MetricsSampler::start(int intervalMs)
{
#ifdef VORSSAINT_HAVE_SENSORS
    int result = VS_OK;
    vs_sensors_system *sys = vs_sensors_system_create(nullptr, &result);
    if (!sys) {
        // The only way this fails is an unreadable /proc/stat, which is worth
        // saying out loud rather than drawing a flat line for.
        m_description = QStringLiteral("placeholder data (/proc/stat unreadable: %1)")
                            .arg(QString::fromUtf8(vs_result_string(result)));
        return false;
    }
    m_sensors = sys;
    m_real = true;
    m_description = QStringLiteral("/proc/stat via vs_sensors");
    // The first call of an instance has no previous sample to subtract, so it
    // is taken now and thrown away; the core count it reports is the label.
    vs_cpu_sample first;
    if (sys->cpu(sys, &first) == VS_OK) {
        m_description = QStringLiteral("/proc/stat via vs_sensors (%1 cores)")
                            .arg(static_cast<qulonglong>(first.core_count));
    }
    m_timer.start(intervalMs);
    return true;
#else
    Q_UNUSED(intervalMs);
    m_description = QStringLiteral("placeholder data (built without vs_sensors)");
    return false;
#endif
}

double MetricsSampler::sampleOnce()
{
#ifdef VORSSAINT_HAVE_SENSORS
    if (!m_sensors)
        return -1;
    auto *sys = static_cast<vs_sensors_system *>(m_sensors);
    vs_cpu_sample sample;
    if (sys->cpu(sys, &sample) != VS_OK || !sample.has_rates)
        return -1;
    const double percent = sample.total_usage * 100.0;
    const QByteArray command = QByteArray("{\"sample\":") + QByteArray::number(percent, 'f', 3) + "}";
    vs_command("metrics", command.constData());
    return percent;
#else
    return -1;
#endif
}
