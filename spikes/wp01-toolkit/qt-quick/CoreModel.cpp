#include "CoreModel.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

extern "C" {
#include "corebridge.h"
}

CoreModel::CoreModel(QObject *parent) : QObject(parent) {}

void CoreModel::setService(const QString &service)
{
    if (m_service == service)
        return;
    m_service = service;
    emit serviceChanged();

    const QByteArray name = m_service.toUtf8();
    if (char *json = vs_snapshot(name.constData())) {
        applySnapshot(QString::fromUtf8(json));
        vs_free(json);
    }
    vs_subscribe(name.constData(), &CoreModel::snapshotTrampoline, this);
}

int CoreModel::invoke(const QString &json)
{
    return vs_command(m_service.toUtf8().constData(), json.toUtf8().constData());
}

void CoreModel::snapshotTrampoline(const char *json, void *ctx)
{
    // The header says the pointer dies with the call, so copy before the hop.
    auto *self = static_cast<CoreModel *>(ctx);
    QMetaObject::invokeMethod(self, "applySnapshot", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(json)));
}

void CoreModel::applySnapshot(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return;
    const QVariantMap next = doc.object().toVariantMap();
    if (next == m_state)
        return; // the Swift side diffs too; this is the belt to its braces
    m_state = next;
    emit stateChanged();
}
