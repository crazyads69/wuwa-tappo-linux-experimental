#include "desktop.h"
#include "platform.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QJsonDocument>
#include <QScreen>
#include <QStandardPaths>
#include <QCoreApplication>
#include <memory>

namespace ww {

namespace kwin {
static QDBusMessage call(const QString &path, const QString &iface, const QString &method, const QVariantList &args) {
    auto m = QDBusMessage::createMethodCall("org.kde.KWin", path, iface, method);
    m.setArguments(args);
    return QDBusConnection::sessionBus().call(m, QDBus::Block, 2000);
}

bool runScript(const QString &name, const QString &source, QString *error) {
    const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR", QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    const QString path = dir + "/" + name + ".js";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(source.toUtf8()) < 0) { if (error) *error = "cannot write script"; return false; }
    f.close();
    unloadScript(name);
    const auto r = call("/Scripting", "org.kde.kwin.Scripting", "loadScript", {path, name});
    const int id = r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty() ? r.arguments().first().toInt() : -1;
    if (id < 0) { if (error) *error = r.errorMessage().isEmpty() ? "KWin refused loadScript" : r.errorMessage(); return false; }
    const auto run = call(QString("/Scripting/Script%1").arg(id), "org.kde.kwin.Script", "run", {});
    if (run.type() == QDBusMessage::ErrorMessage) { if (error) *error = run.errorMessage(); return false; }
    return true;
}

void unloadScript(const QString &name) { call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", {name}); }
}  // namespace kwin

// At most one tiny callback per 1.5 s; stops by itself ~6 s after the app stops acknowledging (crash safety).
static const char *kFullscreenScript = R"JS(
var lastAck = Date.now(), stopped = false, heartbeat = new QTimer(); heartbeat.interval = 1500;
function report() {
  if (stopped) return;
  if (Date.now() - lastAck > 6000) { stopped = true; heartbeat.stop(); return; }
  var w = workspace.activeWindow, yes = !!w && w.pid !== WW_PID && w.fullScreen;
  var g = yes && w.output ? w.output.geometry : {x:0,y:0,width:0,height:0};
  callDBus(WW_BUS, "/Desktop", "org.wuwatappo.Desktop", "Update", JSON.stringify([!!yes,[g.x,g.y,g.width,g.height]]), function() { lastAck = Date.now(); });
}
function watch(w) { w.fullScreenChanged.connect(report); w.outputChanged.connect(report); }
workspace.windowList().forEach(watch); workspace.windowAdded.connect(watch);
workspace.windowActivated.connect(report); workspace.screensChanged.connect(report);
heartbeat.timeout.connect(report); heartbeat.start(); report();
)JS";

static const char *kPanelQuery = R"JS(print(JSON.stringify(panels().map(function(p) {
  var g = screenGeometry(p.screen);
  return {id:p.id, hiding:p.hiding, location:p.location, geometry:[g.x,g.y,g.width,g.height]};
})))JS";

void Desktop::start() {
    if (!platform::kde() || installed_) return;
    const QString bus = QString("org.wuwatappo.Desktop.P%1").arg(QCoreApplication::applicationPid());
    auto session = QDBusConnection::sessionBus();
    if (!session.registerService(bus) || !session.registerObject("/Desktop", this, QDBusConnection::ExportAllSlots)) return;
    QString js = kFullscreenScript;
    js.replace("WW_PID", QString::number(QCoreApplication::applicationPid())).replace("WW_BUS", QJsonDocument(QJsonArray{bus}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
    QString err;
    if (kwin::runScript("wuwatappo-desktop", js, &err)) installed_ = true;
    else qWarning().noquote() << "[desktop] KDE state bridge unavailable:" << err;
}

void Desktop::stop() {
    if (installed_) kwin::unloadScript("wuwatappo-desktop");
    installed_ = false;
    fullscreen_.reset();
}

void Desktop::Update(const QString &payload) {
    fullscreen_ = parseFullscreenReport(payload);
    fsAt_ = Clock::now();
}

void Desktop::refresh(const QString &name) {
    using namespace std::chrono_literals;
    if (refreshing_ || !platform::kde()) return;
    const auto now = Clock::now();
    if (now - availAt_ < 700ms && now - panelsAt_ < 700ms && name == availName_) return;
    refreshing_ = true;
    availName_ = name;
    auto bus = QDBusConnection::sessionBus();
    auto m1 = QDBusMessage::createMethodCall("org.kde.plasmashell", "/StrutManager", "org.kde.PlasmaShell.StrutManager", "availableScreenRect");
    m1 << name;
    auto *w1 = new QDBusPendingCallWatcher(bus.asyncCall(m1, 1000), this);
    auto counter = std::make_shared<int>(2);
    auto fin = [this, counter] { if (--*counter == 0) refreshing_ = false; };
    connect(w1, &QDBusPendingCallWatcher::finished, this, [this, fin](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> r = *w;
        if (!r.isError() && !r.reply().arguments().isEmpty()) {
            const auto arg = r.reply().arguments().first().value<QDBusArgument>();
            int x, y, bw, bh;
            arg.beginStructure(); arg >> x >> y >> bw >> bh; arg.endStructure();
            avail_ = {double(x), double(y), double(bw), double(bh)};
            availAt_ = Clock::now();
        }
        w->deleteLater();
        fin();
    });
    auto m2 = QDBusMessage::createMethodCall("org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell", "evaluateScript");
    m2 << QString(kPanelQuery);
    auto *w2 = new QDBusPendingCallWatcher(bus.asyncCall(m2, 1000), this);
    connect(w2, &QDBusPendingCallWatcher::finished, this, [this, fin](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QString> r = *w;
        if (!r.isError()) { panels_ = QJsonDocument::fromJson(r.value().trimmed().toUtf8()).array(); panelsAt_ = Clock::now(); }
        w->deleteLater();
        fin();
    });
}

static Box boxOf(QScreen *s) { const QRect g = s->geometry(); return {double(g.x()), double(g.y()), double(g.width()), double(g.height())}; }

QJsonObject Desktop::insets(QScreen *s, bool layerCoversOutput) {
    const QJsonObject zero{{"left", 0}, {"top", 0}, {"right", 0}, {"bottom", 0}};
    // Only a surface that covers the whole output needs the panel space reported; a maximized window already excludes it.
    if (!s || !layerCoversOutput || !platform::kde()) return zero;
    refresh(s->name());
    if (availName_ != s->name() || avail_[2] <= 0) return zero;
    return insetsFromRect(boxOf(s), avail_);
}

QJsonObject Desktop::safeArea(QScreen *s, bool layerCoversOutput) {
    using namespace std::chrono_literals;
    QJsonValue edge = QJsonValue::Null;
    bool fs = false;
    if (s) {
        const QString e = autoHideEdge(panels_, boxOf(s));
        if (!e.isEmpty()) edge = e;
        fs = fullscreen_ && Clock::now() - fsAt_ < 3s && sameOutput(*fullscreen_, boxOf(s));
    }
    return {{"insets", insets(s, layerCoversOutput)}, {"autoHideEdge", edge}, {"foregroundFullscreen", fs}};
}

}  // namespace ww
