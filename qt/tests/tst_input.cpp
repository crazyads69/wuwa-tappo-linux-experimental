#include "inputlogic.h"
#include <QtTest>
using namespace ww;

static constexpr int SYN = 0, KEY = 1, ABS = 3;

class TstInput : public QObject {
    Q_OBJECT
    // Feed a protocol-B contact: slot, tracking id (-1 = lift), x, y, then SYN_REPORT at time t.
    static std::optional<int> frame(Tap &tap, quint64 t, int slot, int id, int x = 0, int y = 0) {
        tap.event(t, ABS, kAbsMtSlot, slot);
        tap.event(t, ABS, kAbsMtTrackingId, id);
        if (id >= 0) { tap.event(t, ABS, kAbsMtPosX, x); tap.event(t, ABS, kAbsMtPosY, y); }
        return tap.event(t, SYN, 0, 0);
    }
    static TapCfg cfg() { return {13, 13, 50, 950, false}; }

private slots:
    void keyNamesMatchTheFrontendContract() {
        QCOMPARE(keyName(30), QString("KeyA"));
        QCOMPARE(keyName(57), QString("Space"));
        QCOMPARE(keyName(63), QString("F5"));
        QCOMPARE(keyName(65), QString("F7"));
        QCOMPARE(keyName(66), QString("F8"));
        QCOMPARE(keyName(67), QString("F9"));
        QCOMPARE(keyName(2), QString("Num1"));
        QCOMPARE(keyName(11), QString("Num0"));
        QCOMPARE(keyName(28), QString("Return"));
        QCOMPARE(keyName(29), QString("ControlLeft"));
        QCOMPARE(keyName(44), QString("KeyZ"));
        QCOMPARE(keyName(58), QString("CapsLock"));
        QCOMPARE(keyName(71), QString("Kp7"));
        QCOMPARE(keyName(83), QString("KpDecimal"));
        QCOMPARE(keyName(183), QString("F13"));
        QCOMPARE(keyName(194), QString("F24"));
        QCOMPARE(keyName(125), QString("MetaLeft"));
        QCOMPARE(keyName(250), QString("Unknown(250)"));
    }
    void autorepeatCountsOnceAndReleasesPair() {
        KeyState k;
        QCOMPARE(k.apply(30, 1), std::optional<bool>(true));
        for (int i = 0; i < 100; ++i) QVERIFY(!k.apply(30, 2));  // autorepeat
        QVERIFY(!k.apply(30, 1));                                 // duplicate press
        QCOMPARE(k.apply(30, 0), std::optional<bool>(false));
        QVERIFY(!k.apply(30, 0));                                 // release of a key never seen pressed
        QVERIFY(!k.apply(kKeyBits + 5, 1));                       // out of range
    }
    void resyncAndDrainEmitMissedReleases() {
        KeyState k;
        k.apply(30, 1); k.apply(31, 1);
        std::bitset<kKeyBits> truth;
        truth[31] = true; truth[32] = true;
        const auto missed = k.resync(truth);
        QCOMPARE(missed.size(), 2);  // 30 released, 32 pressed
        QCOMPARE(missed[0], (std::pair<unsigned, bool>{30, false}));
        QCOMPARE(missed[1], (std::pair<unsigned, bool>{32, true}));
        QCOMPARE(k.drain(), (QList<unsigned>{31, 32}));
        QVERIFY(k.held().isEmpty());
    }
    void oneFingerTapIsLeftClick() {
        Tap t(cfg());
        QVERIFY(!frame(t, 1000, 0, 5, 400, 400));
        QCOMPARE(frame(t, 1060, 0, -1), std::optional<int>(0));
    }
    void twoAndThreeFingerTapsAreRightAndMiddle() {
        Tap t(cfg());
        frame(t, 1000, 0, 1, 300, 300);
        frame(t, 1010, 1, 2, 500, 300);
        frame(t, 1060, 0, -1);
        QCOMPARE(frame(t, 1061, 1, -1), std::optional<int>(1));
        frame(t, 2000, 0, 1, 300, 300); frame(t, 2001, 1, 2, 400, 300); frame(t, 2002, 2, 3, 500, 300);
        frame(t, 2050, 0, -1); frame(t, 2051, 1, -1);
        QCOMPARE(frame(t, 2052, 2, -1), std::optional<int>(2));
        TapCfg lmr = cfg(); lmr.lmr = true;
        Tap u(lmr);
        frame(u, 3000, 0, 1, 300, 300); frame(u, 3001, 1, 2, 400, 300);
        frame(u, 3050, 0, -1);
        QCOMPARE(frame(u, 3051, 1, -1), std::optional<int>(2));  // lmr map: two fingers = middle
    }
    void slowOrMovingOrPalmOrClickedContactsAreNotTaps() {
        Tap slow(cfg());
        frame(slow, 1000, 0, 1, 400, 400);
        QVERIFY(!frame(slow, 1000 + kTapTimeoutMs + 1, 0, -1));
        Tap moved(cfg());
        frame(moved, 1000, 0, 1, 400, 400);
        moved.event(1020, ABS, kAbsMtPosX, 400 + 14); // just past 1.3 mm
        moved.event(1020, SYN, 0, 0);
        QVERIFY(!frame(moved, 1050, 0, -1));
        Tap edge(cfg());
        frame(edge, 1000, 0, 1, 10, 400);  // palm edge zone
        QVERIFY(!frame(edge, 1050, 0, -1));
        Tap clicked(cfg());
        frame(clicked, 1000, 0, 1, 400, 400);
        clicked.event(1010, KEY, 0x110, 1);  // physical button cancels
        clicked.event(1020, KEY, 0x110, 0);
        QVERIFY(!frame(clicked, 1050, 0, -1));
        Tap four(cfg());
        for (int i = 0; i < 4; ++i) frame(four, 1000 + i, i, i + 1, 200 + i * 100, 400);
        for (int i = 0; i < 3; ++i) frame(four, 1050 + i, i, -1);
        QVERIFY(!frame(four, 1060, 3, -1));
    }
    void stickyAxesAndReset() {
        Tap t(cfg());
        frame(t, 1000, 0, 1, 400, 400);
        frame(t, 1050, 0, -1);
        // a reused contact need not resend unchanged X/Y
        t.event(2000, ABS, kAbsMtSlot, 0);
        t.event(2000, ABS, kAbsMtTrackingId, 9);
        QVERIFY(!t.event(2000, SYN, 0, 0));
        t.reset();  // SYN_DROPPED: a sequence we cannot see completely is not a tap
        QVERIFY(!frame(t, 2050, 0, -1));
    }
    void moveThresholdFallsBackToA100mmPad() {
        QCOMPARE(tapMoveThreshold(0, 1000, 20), 26);
        QCOMPARE(tapMoveThreshold(0, 1000, 0), 13);
        QCOMPARE(tapMoveThreshold(0, 10, 0), 1);
    }
    void hotkeysAlternate() {
        Hotkeys h;
        QVERIFY(h.raw(1, true));
        QVERIFY(!h.raw(1, true));   // evdev + KWin shortcut both report: forwarded once
        QVERIFY(h.raw(1, false));
        QVERIFY(!h.raw(1, false));
        QCOMPARE(hotkeyIndex("F8"), 1);
        QCOMPARE(hotkeyIndex("KeyA"), -1);
    }
    void domDeliveredButtonSuppressesTheEvdevCopy() {
        using namespace std::chrono;
        const auto t = steady_clock::now();
        QList<Delivered> d{{t + milliseconds(5), "left", "press"}};
        QVERIFY(!takeDelivered(d, t, "right", "press", milliseconds(50)));
        QVERIFY(takeDelivered(d, t, "left", "press", milliseconds(50)));
        QVERIFY(!takeDelivered(d, t, "left", "press", milliseconds(50)));  // consumed once
        d = {{t - milliseconds(500), "left", "press"}};
        QVERIFY(!takeDelivered(d, t, "left", "press", milliseconds(50)));  // too old to belong to this event
    }
};
QTEST_APPLESS_MAIN(TstInput)
#include "tst_input.moc"
