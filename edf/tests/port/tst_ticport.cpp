// SPDX-License-Identifier: GPL-3.0-or-later
//
// TicPort on a pseudo-terminal opened by the test, behind a stable link like the one of the
// simulator: connected only with valid frames, disconnected when silent or gone, reopened
// cleanly. No serial hardware.

#include "ticdecoder.h"
#include "ticport.h"

#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

namespace {

struct Pty {
    int master = -1;
    int slave = -1;
    QString name;
    bool open()
    {
        char buffer[256] = {};
        if (openpty(&master, &slave, buffer, nullptr, nullptr) != 0)
            return false;
        termios settings;
        tcgetattr(slave, &settings);
        cfmakeraw(&settings);
        tcsetattr(slave, TCSANOW, &settings);
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        name = QString::fromLocal8Bit(buffer);
        return true;
    }
    void write(const QByteArray &bytes) const
    {
        if (::write(master, bytes.constData(), bytes.size()) < 0)
            qWarning() << "pty write failed";
    }
    void close()
    {
        if (slave >= 0)
            ::close(slave);
        if (master >= 0)
            ::close(master);
        master = slave = -1;
    }
};

QByteArray frame(const QByteArray &period = "HC..")
{
    return TicDecoder::encodeFrame({ { "ADCO", "000000000000" }, { "OPTARIF", "HC.." }, { "PTEC", period }, { "PAPP", "00450" } });
}

QByteArray parityInBit7(const QByteArray &bytes)
{
    QByteArray out;
    for (char c : bytes) {
        unsigned char b = static_cast<unsigned char>(c) & 0x7F;
        if (__builtin_popcount(b) % 2)
            b |= 0x80;
        out.append(static_cast<char>(b));
    }
    return out;
}

}

class TestTicPort : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void framesGiveConnected();
    void garbageIsNotConnected();
    void silenceGivesDisconnected();
    void absentPortRetried();
    void portGoneAndBack();
    void eightNoneMasked();
    void simulatorOnPty();

private:
    void link(const Pty &pty);
    TicPort *makePort();

    QTemporaryDir *m_dir = nullptr;
    QString m_link;
    Pty m_pty;
    TicPort *m_port = nullptr;
};

void TestTicPort::init()
{
    m_dir = new QTemporaryDir();
    QVERIFY(m_dir->isValid());
    m_link = m_dir->filePath(QStringLiteral("etm-tic"));
    m_pty = Pty();
}

void TestTicPort::cleanup()
{
    delete m_port;
    m_port = nullptr;
    m_pty.close();
    delete m_dir;
}

void TestTicPort::link(const Pty &pty)
{
    QFile::remove(m_link);
    QVERIFY(QFile::link(pty.name, m_link));
}

TicPort *TestTicPort::makePort()
{
    m_port = new TicPort();
    m_port->setPortName(m_link);
    m_port->setStaleAfterMs(400);
    m_port->setReopenAfterMs(1500);
    m_port->setRetryMs(200);
    return m_port;
}

void TestTicPort::framesGiveConnected()
{
    QVERIFY(m_pty.open());
    link(m_pty);
    TicPort *port = makePort();
    QSignalSpy frames(port, &TicPort::frameReceived);
    port->start();
    QTRY_VERIFY(port->portOpen());
    QVERIFY(!port->connected()); // open is not connected
    m_pty.write(frame());
    QTRY_VERIFY(port->connected());
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().first().value<TicReading>().period(), TicReading::Period::OffPeak);
}

void TestTicPort::garbageIsNotConnected()
{
    QVERIFY(m_pty.open());
    link(m_pty);
    TicPort *port = makePort();
    port->start();
    QTRY_VERIFY(port->portOpen());
    // Bytes, even framed, whose checksums are all wrong: not connected.
    QByteArray bad = frame();
    for (int i = 0; i < bad.size(); i++) {
        if (bad.at(i) == '\r')
            bad[i - 1] = static_cast<char>(bad.at(i - 1) == '!' ? '"' : '!');
    }
    for (int i = 0; i < 5; i++)
        m_pty.write(bad + QByteArray("noise\x7f\x7e"));
    QTest::qWait(300);
    QVERIFY(!port->connected());
    QVERIFY(port->decoder().badChecksums() >= 4);
}

void TestTicPort::silenceGivesDisconnected()
{
    QVERIFY(m_pty.open());
    link(m_pty);
    TicPort *port = makePort();
    QSignalSpy changes(port, &TicPort::connectedChanged);
    port->start();
    QTRY_VERIFY(port->portOpen());
    m_pty.write(frame());
    QTRY_VERIFY(port->connected());
    // The meter stops talking (wire cut): disconnected after the stale delay, once.
    QTRY_VERIFY_WITH_TIMEOUT(!port->connected(), 2000);
    QCOMPARE(changes.size(), 2);
    // It talks again: connected again, after a reopen or not.
    QTest::qWait(100);
    QTRY_VERIFY_WITH_TIMEOUT(port->portOpen(), 3000);
    m_pty.write(frame());
    QTRY_VERIFY_WITH_TIMEOUT(port->connected(), 3000);
}

void TestTicPort::absentPortRetried()
{
    TicPort *port = makePort();
    QSignalSpy errors(port, &TicPort::portError);
    port->start();
    QTRY_VERIFY(errors.size() >= 1);
    QVERIFY(!port->portOpen());
    QVERIFY(!port->connected());
    QTest::qWait(700);
    // The same error is reported once, not on every retry.
    QCOMPARE(errors.size(), 1);

    // The simulator starts (or the adapter is plugged in): picked up by the retry.
    QVERIFY(m_pty.open());
    link(m_pty);
    QTRY_VERIFY_WITH_TIMEOUT(port->portOpen(), 3000);
    m_pty.write(frame());
    QTRY_VERIFY(port->connected());
}

void TestTicPort::portGoneAndBack()
{
    QVERIFY(m_pty.open());
    link(m_pty);
    TicPort *port = makePort();
    port->start();
    QTRY_VERIFY(port->portOpen());
    m_pty.write(frame());
    QTRY_VERIFY(port->connected());

    // The pseudo-terminal goes away (simulator stopped).
    m_pty.close();
    QFile::remove(m_link);
    QTRY_VERIFY_WITH_TIMEOUT(!port->connected(), 3000);

    // A new one behind the same link: reopened, connected.
    Pty second;
    QVERIFY(second.open());
    link(second);
    m_pty = second;
    QTRY_VERIFY_WITH_TIMEOUT([&]() { m_pty.write(frame()); return port->connected(); }(), 5000);
}

void TestTicPort::eightNoneMasked()
{
    QVERIFY(m_pty.open());
    link(m_pty);
    TicPort *port = makePort();
    port->setMode(TicPort::Mode::EightNoneMasked);
    QSignalSpy frames(port, &TicPort::frameReceived);
    port->start();
    QTRY_VERIFY(port->portOpen());
    m_pty.write(parityInBit7(frame("HP..")));
    QTRY_VERIFY(port->connected());
    QCOMPARE(frames.first().first().value<TicReading>().period(), TicReading::Period::Peak);
    QCOMPARE(port->decoder().badChecksums(), 0);
}

void TestTicPort::simulatorOnPty()
{
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    const QString simulator = QFINDTESTDATA("../../tools/etm-powersync-tic-simulateur");
    if (python.isEmpty() || simulator.isEmpty())
        QSKIP("python3 or the simulator not found");
    QProcess process;
    process.start(python, { QStringLiteral("-I"), simulator, QStringLiteral("--lien"), m_link, QStringLiteral("--scenario"),
                            QStringLiteral("ejp"), QStringLiteral("--jour-ejp"), QStringLiteral("pointe"), QStringLiteral("--debut"),
                            QStringLiteral("2026-11-20T06:40"), QStringLiteral("--vu-par"), QStringLiteral("8n1"),
                            QStringLiteral("--somme-fausse"), QStringLiteral("2") });
    QVERIFY(process.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(m_link).exists(), 5000);
    TicPort *port = makePort();
    port->setStaleAfterMs(4000);
    QSignalSpy frames(port, &TicPort::frameReceived);
    port->start();
    // Real pace: about 1.5 s per frame at 1200 baud.
    QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 2, 10000);
    QVERIFY(port->connected());
    const TicReading reading = frames.last().first().value<TicReading>();
    QCOMPARE(reading.option(), TicReading::Option::Ejp);
    QVERIFY(reading.ejpNotice());
    QVERIFY(port->decoder().badChecksums() >= 1); // one frame in two has a wrong checksum, dropped group
    process.terminate();
    QVERIFY(process.waitForFinished(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!port->connected(), 8000);
}

QTEST_GUILESS_MAIN(TestTicPort)
#include "tst_ticport.moc"
