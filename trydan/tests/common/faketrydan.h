// SPDX-License-Identifier: GPL-3.0-or-later
//
// A local HTTP server that imitates the Trydan API, for the offline tests: GET
// /RealTimeData and GET /write/<Param>=<int>, HTTP/1.1 with keep-alive.
//
// Behaves like a charger as far as the tests need it: with a vehicle charging and not
// paused, ChargePower and the per-phase currents follow Intensity and ChargeMode; paused,
// they drop to zero while ChargeState stays at 2 - the documented firmware behaviour.
// Open phases carry a small induced current, to exercise the dead band.
//
// Faults on demand: a write refused with another body, a write answered "OK" but not
// applied, a write applied only some readings later, no answer at all, connection refused.

#ifndef FAKETRYDAN_H
#define FAKETRYDAN_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTcpServer>

class QTcpSocket;

class FakeTrydan : public QObject
{
    Q_OBJECT
public:
    explicit FakeTrydan(QObject *parent = nullptr);
    ~FakeTrydan() override;

    bool listen(quint16 port = 0);
    // The charger goes away: listening stops and open connections are dropped.
    void shutdown();
    quint16 port() const { return m_port; }

    // The JSON served, before the charger physics is applied.
    QJsonObject fields;

    // Faults
    QString writeReply = QStringLiteral("OK");  // the body answered to every write
    QSet<QString> ignoredParams;                 // answered "OK", never applied
    int applyAfterReads = 0;                     // a write shows up only after N readings
    bool silent = false;                         // read requests, never answer
    int replyDelayMs = 0;                        // answer every request this late

    // Charger model
    bool physics = true;
    int mixedPhases = 1;           // phases drawn in ChargeMode 2
    double inducedCurrentA = 0.3;  // on an open phase
    bool sendPhaseFields = true;   // false: firmware without IntensityMeasure_* / VoltageMeasure_*
    bool sendChargeMode = true;    // false: firmware without ChargeMode

    // Observation
    QStringList requests;
    int maxConcurrent = 0;
    int writesReceived(const QString &param) const;

    QJsonObject currentReply() const;

    static QJsonObject defaultFields();

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handle(QTcpSocket *socket, const QString &path);
    void respond(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &type);
    void applyPendingOnRead();

    QTcpServer m_server;
    quint16 m_port = 0;
    QList<QTcpSocket *> m_sockets;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    int m_concurrent = 0;
    struct Pending { QString param; int value; int readsLeft; };
    QList<Pending> m_pending;
};

#endif // FAKETRYDAN_H
