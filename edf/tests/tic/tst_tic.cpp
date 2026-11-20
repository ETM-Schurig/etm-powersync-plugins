// SPDX-License-Identifier: GPL-3.0-or-later
//
// The historic TIC decoder and its interpretation, on frames built here and on frames from
// the simulator (tools/etm-powersync-tic-simulateur --dump). No meter, no serial port.

#include "ticdecoder.h"
#include "ticreading.h"

#include <QProcess>
#include <QStandardPaths>
#include <QtTest>

namespace {

TicDecoder::Frame monoBase()
{
    return {
        { "ADCO", "000000000000" }, { "OPTARIF", "BASE" }, { "ISOUSC", "30" }, { "BASE", "012345678" },
        { "PTEC", "TH.." }, { "IINST", "005" }, { "IMAX", "090" }, { "PAPP", "01150" },
        { "HHPHC", "A" }, { "MOTDETAT", "000000" },
    };
}

TicDecoder::Frame triHcHp()
{
    return {
        { "ADCO", "000000000000" }, { "OPTARIF", "HC.." }, { "ISOUSC", "20" }, { "HCHC", "001234567" },
        { "HCHP", "007654321" }, { "PTEC", "HP.." }, { "IINST1", "004" }, { "IINST2", "002" }, { "IINST3", "007" },
        { "IMAX1", "060" }, { "IMAX2", "060" }, { "IMAX3", "060" }, { "PMAX", "13800" }, { "PAPP", "02990" },
        { "HHPHC", "A" }, { "MOTDETAT", "000000" }, { "PPOT", "00" },
    };
}

QByteArray withParityInBit7(const QByteArray &bytes)
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

QString simulator()
{
    return QFINDTESTDATA("../../tools/etm-powersync-tic-simulateur");
}

}

class TestTic : public QObject
{
    Q_OBJECT

private slots:
    void checksumOfKnownGroups_data();
    void checksumOfKnownGroups();
    void singlePhaseFrame();
    void threePhaseFrame();
    void badChecksumDroppedAndCounted();
    void checksumMayBeASpace();
    void interruptedByEot();
    void interruptedByNewStx();
    void joinedMidFrame();
    void parityInBit7();
    void bytesSplitAnywhere();
    void ejpLabels();
    void tempoLabels();
    void standardModeGroupsCounted();
    void garbageWithoutEtxBounded();
    void numbersOnlyDigits();
    void meterNumberNotKept();
    void simulatorFrames_data();
    void simulatorFrames();
};

void TestTic::checksumOfKnownGroups_data()
{
    // Groups as printed in the Enedis documentation and every TIC tutorial.
    QTest::addColumn<QByteArray>("label");
    QTest::addColumn<QByteArray>("data");
    QTest::addColumn<char>("sum");
    QTest::newRow("ISOUSC") << QByteArray("ISOUSC") << QByteArray("30") << '9';
    QTest::newRow("IMAX") << QByteArray("IMAX") << QByteArray("090") << 'H';
    QTest::newRow("HHPHC") << QByteArray("HHPHC") << QByteArray("A") << ',';
    QTest::newRow("MOTDETAT") << QByteArray("MOTDETAT") << QByteArray("000000") << 'B';
    QTest::newRow("OPTARIF HC") << QByteArray("OPTARIF") << QByteArray("HC..") << '<';
}

void TestTic::checksumOfKnownGroups()
{
    QFETCH(QByteArray, label);
    QFETCH(QByteArray, data);
    QFETCH(char, sum);
    QCOMPARE(TicDecoder::checksum(label, data), sum);
}

void TestTic::singlePhaseFrame()
{
    TicDecoder decoder;
    const QList<TicDecoder::Frame> frames = decoder.feed(TicDecoder::encodeFrame(monoBase()));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().size(), 10);
    const TicReading reading = TicReading::fromFrame(frames.first());
    QCOMPARE(reading.option(), TicReading::Option::Base);
    QCOMPARE(reading.period(), TicReading::Period::AllHours);
    QCOMPARE(TicReading::periodLabel(reading.period()), QStringLiteral("Toutes heures"));
    QCOMPARE(reading.subscribedCurrentA().value(), 30);
    QCOMPARE(reading.apparentPowerVA().value(), 1150);
    QCOMPARE(reading.currentA(1).value(), 5);
    QVERIFY(!reading.currentA(2).has_value());
    QVERIFY(!reading.threePhase());
    QCOMPARE(reading.indexWh("BASE").value(), qint64(12345678));
    QVERIFY(!reading.ejpNotice());
    QCOMPARE(decoder.badChecksums(), 0);
    QCOMPARE(decoder.framesDecoded(), 1);
}

void TestTic::threePhaseFrame()
{
    TicDecoder decoder;
    const QList<TicDecoder::Frame> frames = decoder.feed(TicDecoder::encodeFrame(triHcHp()));
    QCOMPARE(frames.size(), 1);
    const TicReading reading = TicReading::fromFrame(frames.first());
    QVERIFY(reading.threePhase());
    QCOMPARE(reading.option(), TicReading::Option::HcHp);
    QCOMPARE(reading.period(), TicReading::Period::Peak);
    QCOMPARE(reading.currentA(1).value(), 4);
    QCOMPARE(reading.currentA(2).value(), 2);
    QCOMPARE(reading.currentA(3).value(), 7);
    QCOMPARE(reading.indexWh("HCHC").value(), qint64(1234567));
    QCOMPARE(reading.indexWh("HCHP").value(), qint64(7654321));
    QCOMPARE(reading.apparentPowerVA().value(), 2990);
}

void TestTic::badChecksumDroppedAndCounted()
{
    QByteArray frame = TicDecoder::encodeFrame(monoBase());
    // Corrupt the checksum of PAPP: the group goes, the frame stays.
    const int at = frame.indexOf("PAPP 01150 ") + 11;
    frame[at] = static_cast<char>(frame.at(at) == '!' ? '"' : '!');
    TicDecoder decoder;
    const QList<TicDecoder::Frame> frames = decoder.feed(frame);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().size(), 9);
    QCOMPARE(decoder.badChecksums(), 1);
    const TicReading reading = TicReading::fromFrame(frames.first());
    QVERIFY(!reading.apparentPowerVA().has_value());
    QCOMPARE(reading.period(), TicReading::Period::AllHours);

    // A corrupted data byte, checksum unchanged: dropped too.
    QByteArray frame2 = TicDecoder::encodeFrame(monoBase());
    frame2[frame2.indexOf("IINST 005") + 8] = '9';
    const QList<TicDecoder::Frame> frames2 = decoder.feed(frame2);
    QCOMPARE(decoder.badChecksums(), 2);
    QVERIFY(!TicReading::fromFrame(frames2.first()).currentA(1).has_value());
}

void TestTic::checksumMayBeASpace()
{
    // Find a data whose checksum is a space, and decode it.
    QByteArray data;
    for (char a = '0'; a <= 'Z' && TicDecoder::checksum("PTEC", data) != ' '; a++) {
        for (char b = '0'; b <= 'Z'; b++) {
            data = QByteArray(1, a) + QByteArray(1, b);
            if (TicDecoder::checksum("PTEC", data) == ' ')
                break;
        }
    }
    QCOMPARE(TicDecoder::checksum("PTEC", data), ' ');
    TicDecoder decoder;
    const QList<TicDecoder::Frame> frames = decoder.feed(TicDecoder::encodeFrame({ { "PTEC", data } }));
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().first().second, data);
    QCOMPARE(decoder.badChecksums(), 0);
}

void TestTic::interruptedByEot()
{
    QByteArray frame = TicDecoder::encodeFrame(monoBase());
    QByteArray cut = frame.left(frame.size() / 2) + '\x04';
    TicDecoder decoder;
    QCOMPARE(decoder.feed(cut).size(), 0);
    QCOMPARE(decoder.interruptedFrames(), 1);
    // The next frame is whole again.
    QCOMPARE(decoder.feed(frame).size(), 1);
}

void TestTic::interruptedByNewStx()
{
    QByteArray frame = TicDecoder::encodeFrame(monoBase());
    TicDecoder decoder;
    const QList<TicDecoder::Frame> frames = decoder.feed(frame.left(40) + frame);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().size(), 10);
    QCOMPARE(decoder.interruptedFrames(), 1);
}

void TestTic::joinedMidFrame()
{
    QByteArray frame = TicDecoder::encodeFrame(monoBase());
    TicDecoder decoder;
    // Port opened in the middle of a frame: its tail is ignored, no partial frame published.
    QCOMPARE(decoder.feed(frame.mid(37)).size(), 0);
    QCOMPARE(decoder.feed(frame).size(), 1);
}

void TestTic::parityInBit7()
{
    TicDecoder decoder;
    const QByteArray raw = withParityInBit7(TicDecoder::encodeFrame(triHcHp()));
    bool anyBit7 = false;
    for (char c : raw)
        anyBit7 |= (static_cast<unsigned char>(c) & 0x80) != 0;
    QVERIFY(anyBit7);
    const QList<TicDecoder::Frame> frames = decoder.feed(raw);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first().size(), triHcHp().size());
    QCOMPARE(decoder.badChecksums(), 0);
}

void TestTic::bytesSplitAnywhere()
{
    const QByteArray stream = TicDecoder::encodeFrame(monoBase()) + TicDecoder::encodeFrame(triHcHp());
    TicDecoder decoder;
    int frames = 0;
    for (char c : stream)
        frames += decoder.feed(QByteArray(1, c)).size();
    QCOMPARE(frames, 2);
}

void TestTic::ejpLabels()
{
    TicDecoder::Frame frame {
        { "ADCO", "000000000000" }, { "OPTARIF", "EJP." }, { "ISOUSC", "45" }, { "EJPHN", "000100000" },
        { "EJPHPM", "000020000" }, { "PEJP", "30" }, { "PTEC", "HN.." }, { "IINST", "010" }, { "PAPP", "02300" },
    };
    TicReading reading = TicReading::fromFrame(TicDecoder().feed(TicDecoder::encodeFrame(frame)).first());
    QCOMPARE(reading.option(), TicReading::Option::Ejp);
    QVERIFY(reading.ejpNotice());
    QCOMPARE(reading.ejpNoticeMinutes(), 30);
    QCOMPARE(reading.period(), TicReading::Period::EjpNormal);
    QCOMPARE(reading.indexWh("EJPHPM").value(), qint64(20000));

    frame.removeAt(5); // no PEJP
    frame[5].second = "PM..";
    reading = TicReading::fromFrame(TicDecoder().feed(TicDecoder::encodeFrame(frame)).first());
    QVERIFY(!reading.ejpNotice());
    QCOMPARE(reading.period(), TicReading::Period::EjpMobilePeak);
    QCOMPARE(TicReading::periodCode(reading.period()), QStringLiteral("pm"));
}

void TestTic::tempoLabels()
{
    const TicDecoder::Frame frame {
        { "OPTARIF", "BBR(" }, { "BBRHCJB", "000001000" }, { "BBRHPJR", "000000500" }, { "PTEC", "HPJR" }, { "DEMAIN", "BLAN" },
    };
    const TicReading reading = TicReading::fromFrame(TicDecoder().feed(TicDecoder::encodeFrame(frame)).first());
    QCOMPARE(reading.option(), TicReading::Option::Tempo);
    QCOMPARE(reading.period(), TicReading::Period::TempoRedPeak);
    QCOMPARE(reading.tempoTomorrow(), TicReading::TempoColor::White);
    QCOMPARE(reading.indexWh("BBRHPJR").value(), qint64(500));
}

void TestTic::standardModeGroupsCounted()
{
    // Standard mode: LF label HT data HT checksum CR. Not decoded, counted.
    const QByteArray standard("\x02\nADSC\t000000000000\tX\r\nVTIC\t02\tJ\r\x03");
    TicDecoder decoder;
    QCOMPARE(decoder.feed(standard).size(), 0);
    QCOMPARE(decoder.standardGroups(), 2);
    QCOMPARE(decoder.framesDecoded(), 0);
}

void TestTic::garbageWithoutEtxBounded()
{
    TicDecoder decoder;
    QByteArray garbage(1, '\x02');
    garbage.append(QByteArray(TicDecoder::MaxFrameBytes + 10, 'Z'));
    QCOMPARE(decoder.feed(garbage).size(), 0);
    QCOMPARE(decoder.interruptedFrames(), 1);
    QCOMPARE(decoder.feed(TicDecoder::encodeFrame(monoBase())).size(), 1);
}

void TestTic::numbersOnlyDigits()
{
    const TicReading reading = TicReading::fromFrame({ { "PAPP", "01A50" }, { "IINST", "-01" }, { "BASE", "" } });
    QVERIFY(!reading.apparentPowerVA().has_value());
    QVERIFY(!reading.currentA(1).has_value());
    QVERIFY(!reading.indexWh("BASE").has_value());
}

void TestTic::meterNumberNotKept()
{
    const TicReading reading = TicReading::fromFrame(monoBase());
    QVERIFY(reading.hasMeterNumber());
    // Nothing of the reading holds the number: not the raw option or period, not an index.
    QVERIFY(!reading.rawOption().contains("000000000000"));
    QVERIFY(!reading.indexesWh().contains("ADCO"));
}

void TestTic::simulatorFrames_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<int>("option");
    QTest::addColumn<int>("period");
    QTest::addColumn<bool>("notice");
    QTest::addColumn<bool>("threePhase");
    QTest::newRow("base mono 7e1") << QStringList { "--scenario", "base" }
                                    << int(TicReading::Option::Base) << int(TicReading::Period::AllHours) << false << false;
    QTest::newRow("hchp tri 8n1, HC at 23:00") << QStringList { "--scenario", "hchp", "--phases", "3", "--vu-par", "8n1", "--debut", "2026-11-20T23:00" }
                                    << int(TicReading::Option::HcHp) << int(TicReading::Period::OffPeak) << false << true;
    QTest::newRow("hchp two ranges, HC at 13:00") << QStringList { "--scenario", "hchp", "--hc", "12:00-14:00,22:00-04:00", "--debut", "2026-11-20T13:00" }
                                    << int(TicReading::Option::HcHp) << int(TicReading::Period::OffPeak) << false << false;
    QTest::newRow("hchp two ranges, HP at 05:00") << QStringList { "--scenario", "hchp", "--hc", "12:00-14:00,22:00-04:00", "--debut", "2026-11-20T05:00" }
                                    << int(TicReading::Option::HcHp) << int(TicReading::Period::Peak) << false << false;
    QTest::newRow("ejp normal day 10:00") << QStringList { "--scenario", "ejp", "--debut", "2026-11-20T10:00" }
                                    << int(TicReading::Option::Ejp) << int(TicReading::Period::EjpNormal) << false << false;
    QTest::newRow("ejp notice 06:30") << QStringList { "--scenario", "ejp", "--jour-ejp", "pointe", "--debut", "2026-11-20T06:30" }
                                    << int(TicReading::Option::Ejp) << int(TicReading::Period::EjpNormal) << true << false;
    QTest::newRow("ejp peak 07:00 tri") << QStringList { "--scenario", "ejp", "--jour-ejp", "pointe", "--phases", "3", "--debut", "2026-11-20T07:00" }
                                    << int(TicReading::Option::Ejp) << int(TicReading::Period::EjpMobilePeak) << false << true;
    QTest::newRow("ejp peak still at 00:59") << QStringList { "--scenario", "ejp", "--dates-pointe", "2026-11-20", "--jour-ejp", "dates", "--debut", "2026-11-21T00:59" }
                                    << int(TicReading::Option::Ejp) << int(TicReading::Period::EjpMobilePeak) << false << false;
    QTest::newRow("ejp peak over at 01:00") << QStringList { "--scenario", "ejp", "--dates-pointe", "2026-11-20", "--jour-ejp", "dates", "--debut", "2026-11-21T01:00" }
                                    << int(TicReading::Option::Ejp) << int(TicReading::Period::EjpNormal) << false << false;
}

void TestTic::simulatorFrames()
{
    QFETCH(QStringList, arguments);
    QFETCH(int, option);
    QFETCH(int, period);
    QFETCH(bool, notice);
    QFETCH(bool, threePhase);
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty() || simulator().isEmpty())
        QSKIP("python3 or the simulator not found");

    QProcess process;
    process.start(python, QStringList { QStringLiteral("-I"), simulator(), QStringLiteral("--dump"), QStringLiteral("3") } + arguments);
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitCode(), 0);
    TicDecoder decoder;
    const QList<TicDecoder::Frame> frames = decoder.feed(process.readAllStandardOutput());
    QCOMPARE(frames.size(), 3);
    QCOMPARE(decoder.badChecksums(), 0);
    const TicReading reading = TicReading::fromFrame(frames.first());
    QCOMPARE(int(reading.option()), option);
    QCOMPARE(int(reading.period()), period);
    QCOMPARE(reading.ejpNotice(), notice);
    QCOMPARE(reading.threePhase(), threePhase);
    QVERIFY(reading.hasMeterNumber());

    // The fault options of the simulator are seen as such.
    QProcess faulty;
    faulty.start(python, QStringList { QStringLiteral("-I"), simulator(), QStringLiteral("--dump"), QStringLiteral("6"),
                                       QStringLiteral("--somme-fausse"), QStringLiteral("2"), QStringLiteral("--coupe"), QStringLiteral("3") } + arguments);
    QVERIFY(faulty.waitForFinished(10000));
    TicDecoder faultyDecoder;
    const QList<TicDecoder::Frame> faultyFrames = faultyDecoder.feed(faulty.readAllStandardOutput());
    QCOMPARE(faultyFrames.size(), 4);           // frames 3 and 6 cut
    QCOMPARE(faultyDecoder.interruptedFrames(), 2);
    QCOMPARE(faultyDecoder.badChecksums(), 2);  // frames 2 and 4 (6 is cut before its bad group)
}

QTEST_GUILESS_MAIN(TestTic)
#include "tst_tic.moc"
