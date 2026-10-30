// Counter store (one JSON file) and maintenance-notice validation, ported from main.rs / service_status.rs.
#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace ww {

class Store {
public:
    explicit Store(QString path) : path_(std::move(path)) {}
    QJsonObject read() const;
    bool set(const QString &key, const QJsonValue &v);  // false on I/O failure (never loses the old file)

private:
    QString path_;
};

// Returns the sanitized notice; on invalid input `error` is set and Null is returned.
// Unknown fields are dropped; titleKey/contentKey are bounded identifiers, groups must be "web".
QJsonValue sanitizeNotice(const QJsonValue &payload, QString *error);

}  // namespace ww
