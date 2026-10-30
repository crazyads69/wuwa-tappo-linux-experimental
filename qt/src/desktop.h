// KDE-only desktop facts, read-only: panel space (Plasma StrutManager), auto-hiding panel edge (plasmashell scripting)
// and "a foreground window is fullscreen on this output" (a tiny KWin script that calls back over D-Bus).
// Every D-Bus call is asynchronous with a short cache, so a hung plasmashell can never freeze the UI thread.
#pragma once
#include "regions.h"
#include <QJsonArray>
#include <QObject>
#include <chrono>

class QScreen;

namespace ww {

namespace kwin {
// Loads and runs a KWin script under `name` (replacing a stale copy). Returns false when KWin is absent/refuses.
bool runScript(const QString &name, const QString &source, QString *error = nullptr);
void unloadScript(const QString &name);
}  // namespace kwin

class Desktop : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.wuwatappo.Desktop")
public:
    explicit Desktop(QObject *parent = nullptr) : QObject(parent) {}
    ~Desktop() override { stop(); }
    void start();  // installs the KWin fullscreen watcher (KDE only; silently skipped elsewhere)
    void stop();
    // {insets, autoHideEdge, foregroundFullscreen} in logical px for `screen`; refreshes the cache in the background.
    QJsonObject safeArea(QScreen *screen, bool layerCoversOutput);
    QJsonObject insets(QScreen *screen, bool layerCoversOutput);

public slots:
    void Update(const QString &payload);  // called by the KWin script

private:
    void refresh(const QString &outputName);
    using Clock = std::chrono::steady_clock;
    Box avail_{};
    QString availName_;
    Clock::time_point availAt_{}, panelsAt_{}, fsAt_{};
    QJsonArray panels_;
    std::optional<Box> fullscreen_;
    bool refreshing_ = false, installed_ = false;
};

}  // namespace ww
