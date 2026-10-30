#include "store.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ww;

static const char *kNotice = R"JSON({"assetsHost":"https://assets.example/app/","titleGroup":"web",
    "titleKey":"maintenance.title","contentGroup":"web","contentKey":"maintenance_body-1","ignored":"unused"})JSON";
static const char *kBad[] = {"[]", "\"text\"", "{\"titleKey\":\"../bad key\"}", "{\"titleKey\":\"\"}", "{\"contentGroup\":\"other\"}",
    "{\"assetsHost\":\"file:///etc/passwd\"}", "{\"assetsHost\":\"javascript:alert(1)\"}",
    "{\"assetsHost\":\"https://user:secret@example.com\"}", "{\"assetsHost\":\"https://example.com/?token=secret\"}", "{\"titleKey\":5}"};

class TstStore : public QObject {
    Q_OBJECT
private slots:
    void persistsAndMerges() {
        QTemporaryDir d;
        Store s(d.filePath("sub/store.json"));
        QVERIFY(s.read().isEmpty());
        QVERIFY(s.set("key_prod", 42));
        QVERIFY(s.set("cum_prod", QJsonObject{{"count", 7}}));
        Store again(d.filePath("sub/store.json"));
        QCOMPARE(again.read()["key_prod"].toInt(), 42);
        QCOMPARE(again.read()["cum_prod"].toObject()["count"].toInt(), 7);
    }
    void corruptFileReadsAsEmpty() {
        QTemporaryDir d;
        QFile f(d.filePath("store.json"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{not json");
        f.close();
        QVERIFY(Store(d.filePath("store.json")).read().isEmpty());
    }
    void noticeDefaultsAndSanitizes() {
        QString err;
        QVERIFY(sanitizeNotice(QJsonValue::Null, &err).isNull());
        QVERIFY(err.isEmpty());
        const auto in = QJsonDocument::fromJson(kNotice).object();
        const auto out = sanitizeNotice(in, &err).toObject();
        QVERIFY(err.isEmpty());
        QCOMPARE(out["assetsHost"].toString(), QString("https://assets.example/app"));
        QVERIFY(!out.contains("ignored"));
        QCOMPARE(out.size(), 5);
        QVERIFY(sanitizeNotice(QJsonObject(), &err).isObject());
    }
    void noticeRejectsBadInput_data() {
        QTest::addColumn<QString>("json");
        for (const char *j : kBad) QTest::newRow(j) << QString(j);
        QTest::newRow("long key") << QString("{\"contentKey\":\"" + QString(129, 'a') + "\"}");
    }
    void noticeRejectsBadInput() {
        QFETCH(QString, json);
        const auto v = QJsonDocument::fromJson("[" + json.toUtf8() + "]").array()[0];
        QString err;
        sanitizeNotice(v, &err);
        QVERIFY2(!err.isEmpty(), qPrintable(json));
    }
};
QTEST_APPLESS_MAIN(TstStore)
#include "tst_store.moc"
