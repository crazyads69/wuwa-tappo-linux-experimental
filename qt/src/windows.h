// Native windows: one QML Window + WebEngineView per Tauri window label. Owns layer-shell setup, stacking policy,
// input regions and monitor placement.
#pragma once
#include "regions.h"
#include <QHash>
#include <QObject>
#include <QQuickWindow>
#include <QRect>
#include <QSet>
#include <QUrl>

class QQmlEngine;
class QScreen;

namespace ww {

enum class Layer { Bottom = 1, Top = 2, Overlay = 3 };  // LayerShellQt::Window::Layer (Background = 0 unused)

// Stacking policy: pets below everything when "always on top" is off, above panels while the panel is raised.
constexpr Layer layerFor(bool alwaysOnTop, bool panelRaised) {
    return !alwaysOnTop ? Layer::Bottom : panelRaised ? Layer::Top : Layer::Overlay;
}

struct WindowOpts {
    QSize size{800, 600};
    QString title;
    bool resizable = false;
    QSize minSize;
    bool pet = false;      // gets the DOM->native mouse shim
    bool layer = false;    // full-output layer-shell surface (when supported)
    Layer layerLevel = Layer::Overlay;
    bool focusable = true;
};

class Windows : public QObject {
    Q_OBJECT
public:
    Windows(QQmlEngine *engine, QObject *shared, QString origin, QObject *parent = nullptr);
    QQuickWindow *get(const QString &label) const { return wins_.value(label); }
    QList<QString> labels() const { return wins_.keys(); }
    // Returns the window and whether it was created now. New windows are built hidden.
    std::pair<QQuickWindow *, bool> create(const QString &label, const QString &route, const WindowOpts &o);
    void destroy(const QString &label);
    bool isLayer(const QString &label) const { return layers_.contains(label); }
    bool isDialogLayer(const QString &label) const { return dialogs_.contains(label); }  // cannot be moved by the client
    void setLayer(const QString &label, Layer l);
    void setInputRects(const QString &label, const QList<QRect> &rects);  // remembered and re-applied after remaps
    void setIgnoreInput(const QString &label, bool ignore);  // set_ignore_cursor_events: whole window click-through
    QScreen *screenWithBounds(const Bounds &b) const;
    static Bounds boundsOf(QScreen *s);  // physical px, GDK-style (origin scaled per output)
    void moveToScreen(const QString &label, QScreen *s);

signals:
    void pageLoaded(const QString &label);
    void windowClosed(const QString &label);
    void externalUrl(const QUrl &url);

private slots:
    void relayPageLoaded();

private:
    void configureLayer(QQuickWindow *w, const WindowOpts &o);
    QQmlEngine *engine_;
    QObject *shared_;
    QString origin_;
    QHash<QString, QQuickWindow *> wins_;
    void applyInput(const QString &label);
    QHash<QString, QList<QRect>> input_;
    QSet<QString> ignore_;
    QHash<QString, QObject *> layers_;  // label -> LayerShellQt::Window (full-output surfaces)
    QSet<QString> dialogs_;             // ordinary windows that had to become anchor-less layer surfaces
};

}  // namespace ww
