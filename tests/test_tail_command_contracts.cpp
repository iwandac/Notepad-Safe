#include <QtTest/QTest>

#include "CommandRegistry.h"
#include "DocumentState.h"
#include "FeatureRegistry.h"

// These are pure document-capability checks, not substitutes for a visible
// Accounts/secure workspace or the runtime's actual admitted Provider gate.
class TestTailCommandContracts : public QObject {
    Q_OBJECT
private slots:
    void tailCommandsAreRegistered()
    {
        const QStringList commands{QStringLiteral("view.tail.toggle"), QStringLiteral("view.tail.resume")};
        for (const auto &command : commands) {
            QCOMPARE(FeatureRegistry::featureIdForCommand(command), QStringLiteral("NSP-LARGEFILE"));
            QVERIFY(FeatureRegistry::isKnownFeature(FeatureRegistry::featureIdForCommand(command)));
            QVERIFY2(FeatureRegistry::requiredStartupCommandIds().contains(command), qPrintable(command));
        }
    }

    void documentPermission_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("allowed");
        const QStringList commands{QStringLiteral("view.tail.toggle"), QStringLiteral("view.tail.resume")};
        const QStringList positive{QStringLiteral("plainText"), QStringLiteral("plainBinary"),
                                   QStringLiteral("ordinaryNone"), QStringLiteral("readonlySaveAs"),
                                   QStringLiteral("recoveredOrdinarySource")};
        const QStringList negative{QStringLiteral("null"), QStringLiteral("editable"),
                                   QStringLiteral("secureClass"), QStringLiteral("accountClass"),
                                   QStringLiteral("securePersistence"), QStringLiteral("noRead"),
                                   QStringLiteral("noSourceLocation")};
        for (const auto &command : commands) {
            for (const auto &kind : positive) {
                const QByteArray name = command.section(QLatin1Char('.'), -1).toLatin1() + '-' + kind.toLatin1();
                QTest::newRow(name.constData()) << command << kind << true;
            }
            for (const auto &kind : negative) {
                const QByteArray name = command.section(QLatin1Char('.'), -1).toLatin1() + '-' + kind.toLatin1();
                QTest::newRow(name.constData()) << command << kind << false;
            }
        }
    }

    void documentPermission()
    {
        QFETCH(QString, command);
        QFETCH(QString, kind);
        QFETCH(bool, allowed);
        // Registration plus positive controls prevents an unknown-command
        // fallback from making the negative matrix pass vacuously.
        QCOMPARE(FeatureRegistry::featureIdForCommand(command), QStringLiteral("NSP-LARGEFILE"));
        DocumentState state;
        state.location = QStringLiteral("C:/tail-contract/ordinary.log");
        state.origin = DocumentOrigin::File;
        state.persistence = DocumentPersistence::PlainFile;
        state.securityClass = DocumentSecurityClass::OrdinaryDocument;
        state.contentKind = DocumentContentKind::Text;
        state.capabilities = DocumentCapability::Read | DocumentCapability::Search | DocumentCapability::Copy;
        state.revision = 71;
        if (kind == QStringLiteral("plainBinary")) state.contentKind = DocumentContentKind::Binary;
        if (kind == QStringLiteral("ordinaryNone")) state.persistence = DocumentPersistence::None;
        if (kind == QStringLiteral("readonlySaveAs")) state.capabilities |= DocumentCapability::Save;
        if (kind == QStringLiteral("recoveredOrdinarySource")) state.origin = DocumentOrigin::Recovery;
        if (kind == QStringLiteral("editable")) state.capabilities |= DocumentCapability::Edit;
        if (kind == QStringLiteral("secureClass")) state.securityClass = DocumentSecurityClass::SecureDocument;
        if (kind == QStringLiteral("accountClass")) state.securityClass = DocumentSecurityClass::AccountSecret;
        if (kind == QStringLiteral("securePersistence")) state.persistence = DocumentPersistence::SecureFile;
        if (kind == QStringLiteral("noRead")) state.capabilities = DocumentCapability::None;
        if (kind == QStringLiteral("noSourceLocation")) state.location.clear();
        const DocumentState before = state;
        const DocumentState *target = kind == QStringLiteral("null") ? nullptr : &state;
        QCOMPARE(CommandRegistry::allowedByDocument(command, target, false), allowed);
        QCOMPARE(CommandRegistry::allowedByDocument(command, target, true), allowed);
        QCOMPARE(state.id, before.id);
        QCOMPARE(state.location, before.location);
        QCOMPARE(state.revision, before.revision);
        QCOMPARE(state.capabilities, before.capabilities);
        QCOMPARE(state.securityClass, before.securityClass);
        QCOMPARE(state.persistence, before.persistence);
        QCOMPARE(state.dirty, before.dirty);
    }
};

QTEST_GUILESS_MAIN(TestTailCommandContracts)
#include "test_tail_command_contracts.moc"
