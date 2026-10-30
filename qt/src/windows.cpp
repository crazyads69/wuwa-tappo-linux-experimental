#include "windows.h"
#include "platform.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScreen>

#ifdef WW_HAVE_LAYERSHELL
#ifdef WW_HAVE_LAYERSHELL_HEADERS
#include <LayerShellQt/Window>
#else
// libLayerShellQtInterface is installed but its development headers may not be: we only call this exported
// static and then use the QObject property system (layer, anchors, exclusionZone, ...), so no layout is mirrored.
namespace LayerShellQt { class Window : public QObject { public: static Window *get(QWindow *); }; }
#endif
#endif

namespace ww {

Windows::Windows(QQmlEngine *engine, QObject *shared, QString origin, QObject *parent)
    : QObject(parent), engine_(engine), shared_(shared), origin_(std::move(origin)) {}

Bounds Windows::boundsOf(QScreen *s) {
    const qreal d = s->devicePixelRatio();
    const QRect g = s->geometry();
    return {int(g.x() * d), int(g.y() * d), unsigned(g.width() * d), unsigned(g.height() * d)};
}

QScreen *Windows::screenWithBounds(const Bounds &b) const {
    for (QScreen *s : QGuiApplication::screens()) if (boundsOf(s) == b) return s;
    return nullptr;
}

std::pair<QQuickWindow *, bool> Windows::create(const QString &label, const QString &route, const WindowOpts &o) {
    if (auto *w = wins_.value(label)) return {w, false};
    QQmlComponent c(engine_, QUrl("qrc:/qml/Page.qml"));
    QObject *obj = c.createWithInitialProperties({{"label", label},
                                                  {"startUrl", QUrl(origin_ + "/index.html" + (route.isEmpty() ? QString() : "#" + route))},
                                                  {"origin", origin_},
                                                  {"shared", QVariant::fromValue(shared_)},
                                                  {"pet", o.pet}});
    auto *w = qobject_cast<QQuickWindow *>(obj);
    if (!w) {
        qCritical() << "cannot create window" << label << c.errorString();
        delete obj;
        return {nullptr, false};
    }
    w->setProperty("wwLabel", label);
    w->setTitle(o.title);
    w->resize(o.size);
    Qt::WindowFlags f = Qt::FramelessWindowHint | Qt::Window;
    if (!o.focusable) f |= Qt::WindowDoesNotAcceptFocus;
    if (!o.layer) f |= Qt::Tool;  // Tool = no taskbar entry on X11 and most Wayland shells
    w->setFlags(f);
    if (o.minSize.isValid()) w->setMinimumSize(o.minSize);
    if (!o.resizable && !o.layer) { w->setMinimumSize(o.size); w->setMaximumSize(o.size); }
    // With the layer-shell Qt integration every window is a layer surface (it has no per-window xdg fallback), so
    // ordinary windows become centered, anchor-less overlay "dialogs". They cannot be dragged (no xdg_toplevel).
    configureLayer(w, o);
    connect(w, SIGNAL(pageLoaded()), this, SLOT(relayPageLoaded()));
    connect(w, SIGNAL(externalUrl(QUrl)), this, SIGNAL(externalUrl(QUrl)));
    connect(w, &QWindow::visibleChanged, this, [this, label](bool v) {
        if (v) applyInput(label);
    });
    wins_.insert(label, w);
    return {w, true};
}

void Windows::configureLayer(QQuickWindow *w, const WindowOpts &o) {
#ifdef WW_HAVE_LAYERSHELL
    if (!platform::layerShellSelected()) return;
    QObject *ls = LayerShellQt::Window::get(w);
    if (!ls) return;
    const QString label = w->property("wwLabel").toString();
    ls->setProperty("scope", "wuwatappo-" + label);
    ls->setProperty("exclusionZone", o.layer ? -1 : 0);  // -1 = ignore panels: the surface covers the whole output
    ls->setProperty("keyboardInteractivity", o.layer || !o.focusable ? 0 : 2);  // none / on demand
    // Anchored to all four edges = the whole output, so window px == monitor px like on Windows; no anchors = centered.
    ls->setProperty("anchors", o.layer ? 15 : 0);
    ls->setProperty("layer", int(o.layer ? o.layerLevel : Layer::Overlay));
    if (o.layer) layers_.insert(label, ls);
    else dialogs_.insert(label);
#else
    Q_UNUSED(w) Q_UNUSED(o)
#endif
}

void Windows::relayPageLoaded() { emit pageLoaded(sender()->property("wwLabel").toString()); }

void Windows::setLayer(const QString &label, Layer l) {
    if (QObject *ls = layers_.value(label)) ls->setProperty("layer", int(l));
}

void Windows::destroy(const QString &label) {
    QQuickWindow *w = wins_.take(label);
    if (!w) return;
    layers_.remove(label);
    dialogs_.remove(label);
    input_.remove(label);
    ignore_.remove(label);
    w->hide();
    w->deleteLater();
    emit windowClosed(label);
}

void Windows::applyInput(const QString &label) {
    QQuickWindow *w = wins_.value(label);
    if (!w || !w->isVisible() || (!input_.contains(label) && !ignore_.contains(label))) return;
    platform::setInputRegion(w, ignore_.contains(label) ? QList<QRect>{} : input_.value(label));
}

void Windows::setInputRects(const QString &label, const QList<QRect> &rects) {
    input_[label] = rects;
    applyInput(label);
}

void Windows::setIgnoreInput(const QString &label, bool ignore) {
    if (ignore) ignore_.insert(label); else ignore_.remove(label);
    applyInput(label);
}

void Windows::moveToScreen(const QString &label, QScreen *s) {
    QQuickWindow *w = wins_.value(label);
    if (!w || !s || w->screen() == s) return;  // setScreen remaps a layer surface, so only when it changes
    // A mapped layer surface keeps the output it was created on (nested KWin, 2 outputs: the page believed it had moved but the
    // surface stayed); unmap, retarget and map again so the compositor receives a new get_layer_surface for the new output.
    const bool remap = isLayer(label) && w->isVisible();
    if (remap) w->destroy();  // hide() keeps the platform window and its old output
    w->setScreen(s);
    if (remap) {
        w->setPosition(s->geometry().center() - QPoint(w->width() / 2, w->height() / 2));  // Qt re-picks the screen from the position on show
        w->show();
    }
    if (!isLayer(label) && platform::x11Session()) w->setPosition(s->geometry().center() - QPoint(w->width() / 2, w->height() / 2));
}

}  // namespace ww
