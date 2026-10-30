#include "regions.h"
#include <QJsonDocument>
#include <QtTest>
using namespace ww;

static const char *kPayload = R"JSON({"regions":[{"left":1,"top":2,"width":3,"height":4,"memberId":"m1"}],
    "inputRegions":[],"updateNativeInput":true,"windowPassthrough":false})JSON";
static const char *kPanels = R"JSON([
  {"id":1,"hiding":"none","location":"top","geometry":[-1200,0,1200,800]},
  {"id":2,"hiding":"autohide","location":"bottom","geometry":[0,0,1200,800]},
  {"id":3,"hiding":"dodgewindows","location":"left","geometry":[-1200,0,1200,800]},
  {"id":4,"hiding":"autohide","location":"right","geometry":[-1200,0,1200,800]}])JSON";

class TstRegions : public QObject {
    Q_OBJECT
private slots:
    void explicitEmptyInputRegionStaysClickThrough() {
        std::optional<Rects> cached;
        const Rects vis{{10, 20, 30, 40}};
        HitRegion h{vis, {}, false, false};
        QCOMPARE(h.inputRects(cached).size(), 1);  // before any input set: fall back to the visual regions
        h.updateNativeInput = true;
        QVERIFY(h.inputRects(cached).isEmpty());   // explicit empty set
        h.updateNativeInput = false;
        QVERIFY(h.inputRects(cached).isEmpty());   // and it sticks
    }
    void inputCacheSurvivesPassthrough() {
        std::optional<Rects> cached;
        HitRegion on{{}, {{1, 2, 3, 4}}, true, true};
        QVERIFY(on.inputRects(cached).isEmpty());  // passthrough wins...
        HitRegion off{{{9, 9, 9, 9}}, {}, false, false};
        QCOMPARE(off.inputRects(cached).size(), 1);  // ...but the cached set comes back
        QCOMPARE(off.inputRects(cached)[0].left, 1.0);
    }
    void parsesFrontendPayload() {
        const auto doc = QJsonDocument::fromJson(kPayload).object();
        const auto h = HitRegion::fromJson(doc);
        QCOMPARE(h.regions.size(), 1);
        QCOMPARE(h.regions[0].memberId.toString(), QString("m1"));
        QVERIFY(h.updateNativeInput);
        QCOMPARE(h.regions[0].toJson()["memberId"].toString(), QString("m1"));
    }
    void pixelRectsCoverTheInclusiveFarEdge() {
        const auto r = toPixelRects({{10.4, 20.6, 30.2, 40.0}});
        QCOMPARE(r[0], QRect(10, 20, 32, 41));
    }
    void collapsedPanelIsClickThroughBeyondTheCard() {
        const Rect r = panelRect(true, 1100, 760);
        QCOMPARE(r.width, 958.0);
        QCOMPARE(r.height, 760.0);
        QCOMPARE(panelRect(true, 1300, 900).width, 1158.0);  // a widened window keeps its extra width
        QCOMPARE(panelRect(true, 100, 760).width, 0.0);      // never negative
        QVERIFY(panelRect(false, 1100, 760).width >= 1100);
    }
    void panelInsetsOnlyCountPanelSpace() {
        for (double x : {-1707.0, 0.0, 1707.0}) {
            const auto o = insetsFromRect({x, -100, 1707, 1067}, {x, -100, 1707, 1005});
            QCOMPARE(o["bottom"].toInt(), 62);
            QCOMPARE(o["left"].toInt() + o["top"].toInt() + o["right"].toInt(), 0);
        }
    }
    void menuAnchorSubtractsOutputOrigin() {
        QCOMPARE(relativePointer(-1720, 500, {-1920, 400}, 2.0), QPointF(100, 50));
        QCOMPARE(relativePointer(3800, 100, {3600, 0}, 2.0), QPointF(100, 50));
    }
    void nearestOptionAndPointerPixels() {
        QCOMPARE(nearest({50, 75, 100, 125, 150, 200}, 90), 100);
        QCOMPARE(nearest({25, 50, 75, 100}, 62), 50);
        QCOMPARE(pointerPixels(101, 51, 1.5), QPoint(152, 77));
    }
    void boundsAndCentering() {
        const Bounds b{-1920, -200, 1920, 1080};
        QVERIFY(b.contains(-1920, -200));
        QVERIFY(b.contains(-1, 879));
        QVERIFY(!b.contains(0, 879));
        QVERIFY(!b.contains(-1, 880));
        QVERIFY(!b.contains(std::nan(""), 0));
        QCOMPARE(centered({-1000, 300, 1000, 800}, 500, 400), QPoint(-750, 500));
        QCOMPARE(centered({-1000, 300, 1000, 800}, 1200, 900), QPoint(-1000, 300));  // larger than the output: no wrap
    }
    void switchGateDeduplicatesAndRecovers() {
        SwitchGate g;
        const auto t0 = SwitchGate::Clock::now();
        const auto first = g.begin(t0);
        QVERIFY(first);
        QVERIFY(!g.begin(t0 + std::chrono::seconds(1)));  // still leased
        g.end(*first);
        QVERIFY(!g.begin(t0 + std::chrono::milliseconds(100)));  // debounce
        QVERIFY(g.begin(t0 + std::chrono::milliseconds(300)));
        const auto t1 = t0 + std::chrono::seconds(5);
        QVERIFY(!g.begin(t1));
        QVERIFY(g.begin(t1 + std::chrono::seconds(16)));  // abandoned lease recovers
    }
    void autoHideMatchesOutputAndMode() {
        const auto panels = QJsonDocument::fromJson(kPanels).array();
        QCOMPARE(autoHideEdge(panels, {-1200, 0, 1200, 800}), QString("right"));
        QCOMPARE(autoHideEdge(panels, {0, 0, 1200, 800}), QString("bottom"));
        QVERIFY(autoHideEdge(panels, {0, 0, 800, 1200}).isEmpty());
        QVERIFY(autoHideEdge(panels, {std::nan(""), 0, 1200, 800}).isEmpty());
    }
    void fullscreenReportIsValidated() {
        QVERIFY(parseFullscreenReport("[true,[-1200,0,1200,800]]"));
        QVERIFY(!parseFullscreenReport("[false,[0,0,0,0]]"));
        QVERIFY(!parseFullscreenReport("[true,[0,0,null,800]]"));
        QVERIFY(!parseFullscreenReport(QString(300, 'x')));
    }
    void networkStatusIsDedupedAndStartsOffline() {
        NetStatusDedupe n;
        QVERIFY(!n.update(false));                // initial state is "no internet": no event
        QCOMPARE(n.update(true), std::optional<bool>(true));
        QVERIFY(!n.update(true));
        QCOMPARE(n.update(false), std::optional<bool>(false));
    }
    void panelClosesOnlyForAnOutsidePressWhenAllowed() {
        QVERIFY(panelOutsideClose(true, false, true, false, true));
        QVERIFY(!panelOutsideClose(false, false, true, false, true));  // no panel
        QVERIFY(!panelOutsideClose(true, true, true, false, true));    // skip flag
        QVERIFY(!panelOutsideClose(true, false, false, false, true));  // pointer unknown (Wayland without a position source)
        QVERIFY(!panelOutsideClose(true, false, true, true, true));    // press inside the panel
        QVERIFY(!panelOutsideClose(true, false, true, false, false));  // grace period
    }
};
QTEST_APPLESS_MAIN(TstRegions)
#include "tst_regions.moc"
