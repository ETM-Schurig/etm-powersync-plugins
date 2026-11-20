// SPDX-License-Identifier: GPL-3.0-or-later

#include "faketrydan.h"

#include <QJsonDocument>
#include <QTcpSocket>
#include <QTimer>

FakeTrydan::FakeTrydan(QObject *parent)
    : QObject(parent),
      fields(defaultFields())
{
    connect(&m_server, &QTcpServer::newConnection, this, &FakeTrydan::onNewConnection);
}

FakeTrydan::~FakeTrydan()
{
    // Before the members go: the sockets would otherwise report their disconnection to a
    // half-destroyed server.
    shutdown();
}

QJsonObject FakeTrydan::defaultFields()
{
    // Values invented for the tests; the field names and types are those of the API.
    QJsonObject o;
    o.insert("ID", "TEST01");
    o.insert("ChargeState", 0);
    o.insert("ReadyState", 0);
    o.insert("ChargePower", 0.0);
    o.insert("ChargeEnergy", 0.0);
    o.insert("SlaveError", 0);
    o.insert("ChargeTime", 0);
    o.insert("HousePower", 0.0);
    o.insert("FVPower", 0.0);
    o.insert("BatteryPower", 0.0);
    o.insert("Paused", 0);
    o.insert("Locked", 0);
    o.insert("Timer", 0);
    o.insert("Intensity", 16);
    o.insert("Dynamic", 0);
    o.insert("MinIntensity", 6);
    o.insert("MaxIntensity", 32);
    o.insert("PauseDynamic", 0);
    o.insert("FirmwareVersion", "9.9.9");
    o.insert("DynamicPowerMode", 0);
    o.insert("ContractedPower", 9000);
    o.insert("ChargeMode", 1);
    for (int i = 1; i <= 3; ++i) {
        o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i), 0.0);
        o.insert(QStringLiteral("VoltageMeasure_L%1").arg(i), 230.0);
    }
    return o;
}

bool FakeTrydan::listen(quint16 port)
{
    if (!m_server.listen(QHostAddress::LocalHost, port))
        return false;
    m_port = m_server.serverPort();
    return true;
}

void FakeTrydan::shutdown()
{
    m_server.close();
    for (QTcpSocket *socket : std::as_const(m_sockets)) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_sockets.clear();
    m_buffers.clear();
    m_concurrent = 0;
}

int FakeTrydan::writesReceived(const QString &param) const
{
    int count = 0;
    for (const QString &request : requests) {
        if (request.startsWith(QStringLiteral("/write/") + param + QLatin1Char('=')))
            ++count;
    }
    return count;
}

QJsonObject FakeTrydan::currentReply() const
{
    QJsonObject o = fields;
    if (physics) {
        const bool flowing = o.value("ChargeState").toInt() == 2 && o.value("Paused").toInt() == 0
                             && o.value("Locked").toInt() == 0;
        int phases = 0;
        if (flowing) {
            switch (o.value("ChargeMode").toInt()) {
            case 0: phases = 1; break;
            case 1: phases = 3; break;
            default: phases = mixedPhases; break;
            }
        }
        const double amps = o.value("Intensity").toInt();
        double power = 0;
        for (int i = 0; i < 3; ++i) {
            const bool active = i < phases;
            o.insert(QStringLiteral("IntensityMeasure_L%1").arg(i + 1), active ? amps - 0.1 : inducedCurrentA);
            if (active)
                power += (amps - 0.1) * 230.0;
        }
        o.insert("ChargePower", power);
    }
    if (!sendPhaseFields) {
        for (int i = 1; i <= 3; ++i) {
            o.remove(QStringLiteral("IntensityMeasure_L%1").arg(i));
            o.remove(QStringLiteral("VoltageMeasure_L%1").arg(i));
        }
    }
    if (!sendChargeMode)
        o.remove("ChargeMode");
    return o;
}

void FakeTrydan::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        m_sockets.append(socket);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_sockets.removeAll(socket);
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void FakeTrydan::onReadyRead(QTcpSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    int end;
    while ((end = buffer.indexOf("\r\n\r\n")) >= 0) {
        const QByteArray head = buffer.left(end);
        buffer.remove(0, end + 4);
        const QList<QByteArray> requestLine = head.split('\n').first().trimmed().split(' ');
        const QString path = requestLine.size() >= 2 ? QString::fromUtf8(requestLine.at(1)) : QString();
        requests.append(path);
        if (silent)
            continue;
        ++m_concurrent;
        maxConcurrent = qMax(maxConcurrent, m_concurrent);
        if (replyDelayMs > 0) {
            QTimer::singleShot(replyDelayMs, socket, [this, socket, path]() { handle(socket, path); });
        } else {
            handle(socket, path);
        }
    }
}

void FakeTrydan::applyPendingOnRead()
{
    for (int i = m_pending.count() - 1; i >= 0; --i) {
        if (--m_pending[i].readsLeft <= 0) {
            fields.insert(m_pending.at(i).param, m_pending.at(i).value);
            m_pending.removeAt(i);
        }
    }
}

void FakeTrydan::handle(QTcpSocket *socket, const QString &path)
{
    --m_concurrent;
    if (path == QLatin1String("/RealTimeData")) {
        applyPendingOnRead();
        respond(socket, 200, QJsonDocument(currentReply()).toJson(QJsonDocument::Compact), "application/json");
        return;
    }
    if (path.startsWith(QLatin1String("/write/"))) {
        const QString assignment = path.mid(7);
        const int eq = assignment.indexOf(QLatin1Char('='));
        bool ok = false;
        const QString param = assignment.left(eq);
        const int value = assignment.mid(eq + 1).toInt(&ok);
        if (eq <= 0 || !ok || !fields.contains(param)) {
            respond(socket, 200, "ERROR", "text/plain");
            return;
        }
        if (writeReply == QLatin1String("OK") && !ignoredParams.contains(param)) {
            if (applyAfterReads > 0)
                m_pending.append({param, value, applyAfterReads});
            else
                fields.insert(param, value);
        }
        respond(socket, 200, writeReply.toUtf8(), "text/plain");
        return;
    }
    respond(socket, 404, "Not found", "text/plain");
}

void FakeTrydan::respond(QTcpSocket *socket, int status, const QByteArray &body, const QByteArray &type)
{
    if (!m_sockets.contains(socket))
        return;
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Not Found") + "\r\n";
    response += "Content-Type: " + type + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: keep-alive\r\n\r\n";
    response += body;
    socket->write(response);
}
