// CoreModel - the one generic QML binding PLAN.md 4.2 calls for: a QObject
// with a QVariantMap `state` (the service's decoded snapshot) and
// invoke(QString json) (the service's Codable command enum, as JSON).
// One instance per service; no view-specific bindings anywhere.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class CoreModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit CoreModel(QObject *parent = nullptr);

    QString service() const { return m_service; }
    void setService(const QString &service);
    QVariantMap state() const { return m_state; }

    // Send a command to the bound service. Returns the bridge's status code.
    Q_INVOKABLE int invoke(const QString &json);

    // Called on the bridge's thread; hops to the GUI thread itself.
    static void snapshotTrampoline(const char *json, void *ctx);

public slots:
    // Queued from snapshotTrampoline, so it always runs on the GUI thread.
    void applySnapshot(const QString &json);

signals:
    void serviceChanged();
    void stateChanged();

private:
    QString m_service;
    QVariantMap m_state;
};
