#include "StartupTrace.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

// Serialization/enablement contracts only. These deliberately call record()
// directly and are not evidence of actual first paint, input or performance.
class StartupTraceTest final : public QObject {
    Q_OBJECT
private:
    QByteArray previous_;
    bool wasSet_ = false;

    static QJsonObject json(const StartupTrace &trace)
    {
        return QJsonDocument::fromJson(trace.toJson(0)).object();
    }

    static void complete(StartupTrace &trace, StartupTrace::PaintCounts counts = {})
    {
        using Stage = StartupTrace::Stage;
        trace.record(Stage::AppReady);
        trace.record(Stage::CompositionReady);
        trace.record(Stage::WindowReady);
        trace.record(Stage::ShowBegin);
        trace.record(Stage::FirstPaint); // paint inside show() is legal
        trace.recordPaintCounts(counts);
        trace.record(Stage::ShowReturned);
        trace.record(Stage::PostPaintIdleTurn);
        trace.record(Stage::RestoreCompleted);
        trace.record(Stage::EventLoopFinished);
    }

private slots:
    void init()
    {
        wasSet_ = qEnvironmentVariableIsSet("NOTEPADSAFE_STARTUP_TRACE");
        previous_ = qgetenv("NOTEPADSAFE_STARTUP_TRACE");
        qunsetenv("NOTEPADSAFE_STARTUP_TRACE");
    }

    void cleanup()
    {
        if (wasSet_)
            qputenv("NOTEPADSAFE_STARTUP_TRACE", previous_);
        else
            qunsetenv("NOTEPADSAFE_STARTUP_TRACE");
    }

    void disabledProducesNoPayload()
    {
        StartupTrace trace;
        QVERIFY(!trace.enabled());
        for (int index = 0; index < 32; ++index)
            complete(trace, {1, 2, 3, 4, 5});
        QVERIFY(trace.toJson(0).isEmpty());
        QVERIFY(trace.toJson(7).isEmpty());
    }

    void explicitEnablement_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("empty") << QByteArray("") << false;
        QTest::newRow("zero") << QByteArray("0") << false;
        QTest::newRow("true") << QByteArray("true") << false;
        QTest::newRow("prefix") << QByteArray("01") << false;
        QTest::newRow("suffix") << QByteArray("1 ") << false;
        QTest::newRow("leading-space") << QByteArray(" 1") << false;
        QTest::newRow("exact") << QByteArray("1") << true;
    }

    void explicitEnablement()
    {
        QFETCH(QByteArray, value);
        QFETCH(bool, enabled);
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", value));
        StartupTrace trace;
        QCOMPARE(trace.enabled(), enabled);
        complete(trace);
        QCOMPARE(trace.toJson(0).isEmpty(), !enabled);
    }

    void completePayloadIsBoundedAndNumeric()
    {
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", "1"));
        StartupTrace trace;
        complete(trace, {1, 2, 3, 4, 5});
        const QByteArray payload = trace.toJson(0);
        QVERIFY(payload.size() < 8192);
        QJsonParseError parseError;
        const QJsonObject result = QJsonDocument::fromJson(payload, &parseError).object();
        QCOMPARE(parseError.error, QJsonParseError::NoError);
        QCOMPARE(result.keys(), (QStringList{"complete", "duplicate_stage", "events", "exit_code",
                                             "invalid_clock", "invalid_stage", "overflow", "paint_counts",
                                             "pid", "qpc_frequency", "schema", "valid"}));
        QCOMPARE(result.value("schema").toString(), QStringLiteral("startup-trace-v1"));
        QCOMPARE(result.value("pid").toInteger(), QCoreApplication::applicationPid());
        QCOMPARE(result.value("exit_code").toInt(-1), 0);
        QVERIFY(result.value("valid").toBool()); // not a zero-factory/performance verdict
        QVERIFY(!result.value("invalid_clock").toBool(true));
        QVERIFY(!result.value("overflow").toBool(true));
        QVERIFY(!result.value("duplicate_stage").toBool(true));
        QVERIFY(!result.value("invalid_stage").toBool(true));
        bool frequencyOk = false;
        QVERIFY(result.value("qpc_frequency").isString());
        QVERIFY(result.value("qpc_frequency").toString().toLongLong(&frequencyOk) > 0);
        QVERIFY(frequencyOk);
        const QJsonObject counts = result.value("paint_counts").toObject();
        QCOMPARE(counts.keys(), (QStringList{"documents", "heavy", "inference", "persistence", "tools"}));
        QCOMPARE(counts.value("heavy").toInt(), 1);
        QCOMPARE(counts.value("tools").toInt(), 2);
        QCOMPARE(counts.value("documents").toInt(), 3);
        QCOMPARE(counts.value("persistence").toInt(), 4);
        QCOMPARE(counts.value("inference").toInt(), 5);
        const QJsonArray events = result.value("events").toArray();
        QCOMPARE(events.size(), qsizetype(9));
        const QStringList expected{"application_ready", "composition_ready", "window_ready",
                                   "show_begin", "first_paint", "show_returned", "post_paint_idle",
                                   "restore_completed", "event_loop_finished"};
        qint64 previous = 0;
        for (qsizetype index = 0; index < events.size(); ++index) {
            const QJsonObject event = events.at(index).toObject();
            QCOMPARE(event.keys(), (QStringList{"stage", "ticks"}));
            QCOMPARE(event.value("stage").toString(), expected.at(index));
            QVERIFY(event.value("ticks").isString());
            bool ok = false;
            const qint64 ticks = event.value("ticks").toString().toLongLong(&ok);
            QVERIFY(ok && ticks > 0 && ticks >= previous);
            previous = ticks;
        }
        QCOMPARE(trace.toJson(0), payload); // export itself cannot append a stage
        QCOMPARE(QJsonDocument::fromJson(trace.toJson(7)).object().value("exit_code").toInt(), 7);
    }

    void incompleteCannotPretendToBeFinished()
    {
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", "1"));
        StartupTrace trace;
        trace.record(StartupTrace::Stage::AppReady);
        const QJsonObject result = json(trace);
        QVERIFY(!result.value("valid").toBool(true));
        QVERIFY(result.value("paint_counts").isNull());
        QCOMPARE(result.value("events").toArray().size(), qsizetype(1));
    }

    void duplicateIsNotSilentlyReplaced()
    {
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", "1"));
        StartupTrace trace;
        complete(trace);
        const QJsonValue original = json(trace).value("events").toArray().first();
        trace.record(StartupTrace::Stage::AppReady);
        const QJsonObject result = json(trace);
        QVERIFY(!result.value("valid").toBool(true));
        QVERIFY(result.value("duplicate_stage").toBool());
        QCOMPARE(result.value("events").toArray().first(), original);
    }

    void overflowIsBoundedAndExplicit()
    {
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", "1"));
        StartupTrace trace;
        for (std::size_t index = 0; index < StartupTrace::Capacity + 5; ++index)
            trace.record(StartupTrace::Stage::AppReady);
        const QJsonObject result = json(trace);
        QVERIFY(!result.value("valid").toBool(true));
        QVERIFY(result.value("overflow").toBool());
        QCOMPARE(result.value("events").toArray().size(), qsizetype(StartupTrace::Capacity));
        QVERIFY(trace.toJson(0).size() < 8192);
    }

    void duplicateCountsKeepFirstObservation()
    {
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", "1"));
        StartupTrace trace;
        complete(trace, {1, 2, 3, 4, 5});
        const QJsonValue original = json(trace).value("paint_counts");
        trace.recordPaintCounts({6, 7, 8, 9, 10});
        const QJsonObject result = json(trace);
        QVERIFY(!result.value("valid").toBool(true));
        QCOMPARE(result.value("paint_counts"), original);
    }

    void invalidEnumFailsClosed()
    {
        QVERIFY(qputenv("NOTEPADSAFE_STARTUP_TRACE", "1"));
        StartupTrace trace;
        complete(trace);
        trace.record(static_cast<StartupTrace::Stage>(99));
        const QJsonObject result = json(trace);
        QVERIFY(!result.value("valid").toBool(true));
        QVERIFY(result.value("invalid_stage").toBool());
        QVERIFY(result.value("events").toArray().size() <= qsizetype(StartupTrace::Capacity));
    }
};

QTEST_GUILESS_MAIN(StartupTraceTest)
#include "test_startup_trace.moc"
