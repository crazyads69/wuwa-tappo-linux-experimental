#include "store.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

namespace ww {

QJsonObject Store::read() const {
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool Store::set(const QString &key, const QJsonValue &v) {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    auto o = read();
    o[key] = v;
    QSaveFile f(path_);  // atomic replace
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0 && f.commit();
}

static bool identOk(const QString &s) {
    if (s.isEmpty() || s.size() > 128) return false;
    for (QChar c : s)
        if (!(c.unicode() < 128 && (c.isLetterOrNumber() || QStringLiteral("_.-").contains(c)))) return false;
    return true;
}

QJsonValue sanitizeNotice(const QJsonValue &payload, QString *error) {
    auto fail = [&](const char *m) { if (error) *error = m; return QJsonValue(); };
    if (payload.isNull() || payload.isUndefined()) return QJsonValue::Null;
    if (!payload.isObject()) return fail("maintenance payload must be an object or null");
    const auto src = payload.toObject();
    QJsonObject out;
    for (auto key : {"titleKey", "contentKey"}) {
        if (!src.contains(key)) continue;
        if (!src[key].isString()) return fail("maintenance dictionary key must be text");
        const QString t = src[key].toString().trimmed();
        if (!identOk(t)) return fail("invalid maintenance dictionary key");
        out[key] = t;
    }
    for (auto key : {"titleGroup", "contentGroup"}) {
        if (!src.contains(key)) continue;
        if (src[key].toString() != "web") return fail("invalid maintenance dictionary group");
        out[key] = "web";
    }
    if (src.contains("assetsHost")) {
        if (!src["assetsHost"].isString()) return fail("maintenance assets host must be text");
        const QString text = src["assetsHost"].toString();
        const QUrl u(text, QUrl::StrictMode);
        if (!u.isValid() || text.size() > 2048 || (u.scheme() != "https" && u.scheme() != "http") || u.host().isEmpty()
            || !u.userName().isEmpty() || !u.password().isEmpty() || u.hasQuery() || u.hasFragment())
            return fail("invalid maintenance assets host");
        QString s = u.toString(QUrl::FullyEncoded);
        while (s.endsWith('/')) s.chop(1);
        out["assetsHost"] = s;
    }
    return out;
}

}  // namespace ww
