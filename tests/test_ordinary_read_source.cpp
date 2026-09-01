#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

#include "AppComposition.h"
#include "DiskFileVersion.h"
#include "OrdinaryReadSource.h"
#include "ReadOnlyTextAnalysis.h"
#include "PersistenceAdapterRegistry.h"
#include "SharedReadOnlyFile.h"

namespace {

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// QSaveFile uses MoveFileExW, which cannot replace this still-open target on
// the tested Windows host (raw Win32 and SharedReadOnlyFile behave equally).
// Exercise an actual external POSIX-semantic rename without closing the old
// reader; this fixture does not change the product's save implementation.
bool replaceOpenTarget(const QString &replacement, const QString &target, QString *error)
{
#if defined(Q_OS_WIN) && defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && NTDDI_VERSION >= NTDDI_WIN10_RS1
    const HANDLE source = ::CreateFileW(reinterpret_cast<LPCWSTR>(replacement.utf16()),
        DELETE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) {
        const DWORD nativeError = ::GetLastError();
        qInfo().noquote() << "RENAME_SOURCE_OPEN" << replacement << "BOOL=0 error=" << nativeError;
        *error = QStringLiteral("CreateFileW: %1").arg(nativeError);
        return false;
    }
    const QString nativeTarget = QDir::toNativeSeparators(target);
    const DWORD nameBytes = DWORD(nativeTarget.size() * sizeof(wchar_t));
    // Include the full structure and a zeroed trailing wchar; FileNameLength
    // still counts only the intended name, excluding its terminator.
    const size_t bufferBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    const std::unique_ptr<void, void (*)(void *)> buffer(
        ::operator new(bufferBytes), [](void *p) { ::operator delete(p); });
    std::memset(buffer.get(), 0, bufferBytes);
    auto *info = static_cast<FILE_RENAME_INFO *>(buffer.get());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->FileNameLength = nameBytes;
    std::memcpy(info->FileName, nativeTarget.utf16(), nameBytes);
    ::SetLastError(ERROR_SUCCESS);
    const BOOL renamed = ::SetFileInformationByHandle(source, FileRenameInfoEx, info, DWORD(bufferBytes));
    const DWORD nativeError = ::GetLastError();
    wchar_t finalPath[32768]{};
    const DWORD pathLength = ::GetFinalPathNameByHandleW(source, finalPath, 32768, FILE_NAME_NORMALIZED);
    const QString actualTarget = pathLength > 0 && pathLength < 32768
        ? QString::fromWCharArray(finalPath, pathLength) : QStringLiteral("unavailable");
    const QString expectedTarget = QStringLiteral("\\\\?\\") + nativeTarget;
    qInfo().noquote() << "RENAME_PRIMITIVE FileRenameInfoEx BOOL=" << renamed
        << "GetLastError=" << nativeError << "source=" << replacement
        << "expected-target=" << expectedTarget << "actual-target=" << actualTarget;
    ::CloseHandle(source);
    if (!renamed) *error = QStringLiteral("FileRenameInfoEx: %1").arg(nativeError);
    else if (actualTarget != expectedTarget)
        *error = QStringLiteral("Rename target mismatch: expected %1, actual %2")
            .arg(expectedTarget, actualTarget);
    return renamed != FALSE && actualTarget == expectedTarget;
#else
    Q_UNUSED(replacement);
    Q_UNUSED(target);
    *error = QStringLiteral("Native POSIX-semantic rename is unavailable in this SDK.");
    return false;
#endif
}

bool setModifiedTime(const QString &path, qint64 milliseconds)
{
    QFile file(path);
    return file.open(QIODevice::ReadWrite)
        && file.setFileTime(QDateTime::fromMSecsSinceEpoch(milliseconds),
                            QFileDevice::FileModificationTime);
}

// Deliberately not a copy of production KDBX/suffix recognition. This custom
// pure rule tests snapshot ownership; production semantics use AppComposition.
bool recognizesTestSecure(const QString &, QByteArrayView prefix)
{
    return prefix == QByteArrayView("TEST-LOCK", 9);
}

PersistenceAdapterDescriptor testSecureDescriptor()
{
    return {QStringLiteral("test.secure"), QStringLiteral("Test secure route"),
            100, DocumentPersistence::SecureFile};
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// Independent oracle for the pre-existing save fingerprint format. Do not
// call captureDiskFileSampleHash here: both production capture APIs share it.
QByteArray expectedSampleHash(const QByteArray &bytes)
{
    constexpr qint64 sampleBytes = 64 * 1024;
    QByteArray encoded = QByteArray::number(bytes.size());
    const auto appendSample = [&](qint64 offset, qint64 length) {
        encoded += QByteArray::number(offset);
        encoded += ':';
        encoded += bytes.mid(offset, length);
    };
    if (bytes.size() <= 3 * sampleBytes) {
        appendSample(0, bytes.size());
    } else {
        appendSample(0, sampleBytes);
        appendSample(bytes.size() / 2 - sampleBytes / 2, sampleBytes);
        appendSample(bytes.size() - sampleBytes, sampleBytes);
    }
    return QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
}

} // namespace

class TestOrdinaryReadSource : public QObject {
    Q_OBJECT
private slots:
    void atomicReplacementKeepsOldDescriptorButRejectsOldExpectedStamp()
    {
#ifndef Q_OS_WIN
        QSKIP("Native file-identity and atomic replacement acceptance targets Windows.");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("ordinary.dat"));
        const QByteArray original(8192, 'A');
        const QByteArray replacement(8192, 'B');
        QVERIFY(writeBytes(path, original));
        AppComposition composition;
        const auto policy = composition.persistenceAdapters().readPolicySnapshot();
        QVERIFY(policy.isAvailable());
        SharedReadOnlyFile oldReader;
        const auto opened = openOrdinaryReadSource(oldReader, path, policy);
        QVERIFY2(opened.isReady(), qPrintable(opened.error));
        QVERIFY(opened.stamp.valid && !opened.stamp.fileIdentity.isEmpty());
        DiskFileVersion originalVersion;
        QString error;
        QVERIFY2(oldReader.captureVersion(&originalVersion, &error), qPrintable(error));

        const QString replacementPath = directory.filePath(QStringLiteral("replacement.dat"));
        QVERIFY(writeBytes(replacementPath, replacement));
        const bool replaced = replaceOpenTarget(replacementPath, path, &error);
        QVERIFY2(replaced, qPrintable(error));
        QVERIFY(oldReader.isOpen());
        const bool modifiedTimeSet = setModifiedTime(path, opened.stamp.modifiedMs);
        QVERIFY(modifiedTimeSet);
        DiskFileStamp current;
        QCOMPARE(captureDiskFileStamp(path, &current, &error), DiskFileState::Unchanged);
        qInfo().noquote() << "RENAME_STAMPS old=" << opened.stamp.fileIdentity
            << "current=" << current.fileIdentity << "size=" << current.size
            << "old-mtime=" << opened.stamp.modifiedMs << "current-mtime=" << current.modifiedMs;
        QCOMPARE(current.size, opened.stamp.size);
        QCOMPARE(current.modifiedMs, opened.stamp.modifiedMs);
        QVERIFY(!current.fileIdentity.isEmpty());
        QVERIFY(current.fileIdentity != opened.stamp.fileIdentity);
        QVERIFY(!sameDiskFileStamp(current, opened.stamp));
        QCOMPARE(readBytes(path), replacement);

        error.clear();
        QCOMPARE(oldReader.readAt(0, original.size(), &error), original);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        auto *oldMapping = oldReader.map(0, original.size());
        QVERIFY2(oldMapping, qPrintable(oldReader.errorString()));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(oldMapping), original.size()), original);
        QVERIFY(oldReader.unmap(oldMapping));
        DiskFileStamp oldStampAgain;
        QVERIFY(oldReader.captureStamp(&oldStampAgain, &error));
        QVERIFY(sameDiskFileStamp(oldStampAgain, opened.stamp));
        DiskFileVersion oldVersionAgain;
        QVERIFY(oldReader.captureVersion(&oldVersionAgain, &error));
        QCOMPARE(oldVersionAgain.sampleHash, originalVersion.sampleHash);
        QCOMPARE(oldVersionAgain.fileIdentity, originalVersion.fileIdentity);

        SharedReadOnlyFile rejected;
        const auto stale = openOrdinaryReadSource(rejected, path, policy, &opened.stamp);
        QCOMPARE(stale.status, OrdinaryReadStatus::Changed);
        QVERIFY(!stale.isReady() && !rejected.isOpen() && !stale.error.isEmpty());
        QCOMPARE(rejected.readAt(0, replacement.size()), QByteArray{});
        SharedReadOnlyFile newReader;
        const auto accepted = openOrdinaryReadSource(newReader, path, policy, &current);
        QVERIFY2(accepted.isReady(), qPrintable(accepted.error));
        QCOMPARE(newReader.readAt(0, replacement.size()), replacement);
        QCOMPARE(readBytes(path), replacement);
        QCOMPARE(composition.heavyFactoryCalls(), 0);
        QCOMPARE(composition.persistenceAdapters().instantiatedCount(), 0);
    }

    void registeredFormatSemantics_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QString>("fixtureName");
        QTest::addColumn<int>("variant");
        QTest::addColumn<bool>("secure");
        QTest::newRow("real-kdbx3-header-in-dat") << QStringLiteral("ordinary.dat")
            << QStringLiteral("Format300.kdbx") << 0 << true;
        QTest::newRow("real-kdbx4-header-in-dat") << QStringLiteral("ordinary.dat")
            << QStringLiteral("Format400.kdbx") << 0 << true;
        QTest::newRow("nsafe-suffix") << QStringLiteral("ordinary.nsafe") << QString{} << 0 << true;
        QTest::newRow("kdbx-suffix") << QStringLiteral("ordinary.kdbx") << QString{} << 0 << true;
        QTest::newRow("uppercase-nsafe-suffix") << QStringLiteral("ordinary.NSAFE") << QString{} << 0 << true;
        QTest::newRow("uppercase-kdbx-suffix") << QStringLiteral("ordinary.KDBX") << QString{} << 0 << true;
        QTest::newRow("unknown-major-keeps-existing-recognition") << QStringLiteral("ordinary.dat")
            << QStringLiteral("Format400.kdbx") << 1 << false;
        QTest::newRow("short-header-keeps-existing-recognition") << QStringLiteral("ordinary.dat")
            << QStringLiteral("Format400.kdbx") << 2 << false;
        QTest::newRow("ordinary-bytes") << QStringLiteral("ordinary.dat") << QString{} << 0 << false;
    }

    void registeredFormatSemantics()
    {
        QFETCH(QString, fileName);
        QFETCH(QString, fixtureName);
        QFETCH(int, variant);
        QFETCH(bool, secure);
        QByteArray bytes("ordinary source bytes\n");
        if (!fixtureName.isEmpty()) {
            const QString relative = QStringLiteral("fixtures/kdbx/") + fixtureName;
            const QString fixture = QFINDTESTDATA(qPrintable(relative));
            QVERIFY(!fixture.isEmpty());
            bytes = readBytes(fixture);
            QVERIFY(bytes.size() > 12);
        }
        if (variant == 1) {
            bytes[10] = char(5); // Unknown major is not newly classified as KDBX.
            bytes[11] = char(0);
        } else if (variant == 2) {
            bytes.truncate(11);
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(fileName);
        QVERIFY(writeBytes(path, bytes));
        AppComposition composition;
        const auto policy = composition.persistenceAdapters().readPolicySnapshot();
        QVERIFY(policy.isAvailable());
        QCOMPARE(policy.requiredPrefixBytes(), qsizetype(12));
        SharedReadOnlyFile reader;
        const auto result = openOrdinaryReadSource(reader, path, policy);
        QCOMPARE(result.status, secure ? OrdinaryReadStatus::SecureType : OrdinaryReadStatus::Ready);
        QCOMPARE(result.isReady(), !secure);
        QCOMPARE(reader.isOpen(), !secure);
        if (secure) {
            QCOMPARE(result.adapterId, QStringLiteral("secure.kdbx"));
            QVERIFY(!result.error.isEmpty());
            QCOMPARE(reader.readAt(0, bytes.size()), QByteArray{});
            // A format refusal remains a format refusal even when the caller's
            // expected metadata also differs; no ordinary byte reader escapes.
            DiskFileStamp wrong;
            wrong.valid = true;
            wrong.size = bytes.size() + 1;
            const auto stillSecure = openOrdinaryReadSource(reader, path, policy, &wrong);
            QCOMPARE(stillSecure.status, OrdinaryReadStatus::SecureType);
            QVERIFY(!reader.isOpen());
        } else {
            QVERIFY(result.adapterId.isEmpty());
            QCOMPARE(reader.readAt(0, bytes.size()), bytes);
        }
        QCOMPARE(readBytes(path), bytes);
        QCOMPARE(composition.heavyFactoryCalls(), 0);
        QCOMPARE(composition.persistenceAdapters().instantiatedCount(), 0);
        // These are format-admission assertions, not decryption or generic
        // guarantees for currently unrecognized security formats/versions.
    }

    void emptyOrdinaryFileIsReady()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("empty.txt"));
        QVERIFY(writeBytes(path, {}));
        AppComposition composition;
        SharedReadOnlyFile reader;
        const auto result = openOrdinaryReadSource(
            reader, path, composition.persistenceAdapters().readPolicySnapshot());
        QVERIFY2(result.isReady(), qPrintable(result.error));
        QVERIFY(reader.isOpen() && result.stamp.valid && result.error.isEmpty());
        QCOMPARE(result.stamp.size, qint64(0));
        QString error;
        QCOMPARE(reader.readAt(0, 1, &error), QByteArray{});
        QVERIFY(error.isEmpty());
        DiskFileStamp again;
        QVERIFY(reader.captureStamp(&again, &error));
        QVERIFY(sameDiskFileStamp(again, result.stamp));
        QCOMPARE(composition.heavyFactoryCalls(), 0);
    }

    void unavailablePolicyFailsClosed_data()
    {
        QTest::addColumn<int>("configuration");
        QTest::newRow("empty-registry") << 0;
        QTest::newRow("secure-route-without-header-rule") << 1;
        QTest::newRow("one-valid-and-one-missing-secure-rule") << 2;
        QTest::newRow("factory-only-secure-adapter") << 3;
        QTest::newRow("valid-probe-zero-prefix") << 4;
        QTest::newRow("valid-probe-negative-prefix") << 5;
        QTest::newRow("valid-probe-oversized-prefix") << 6;
    }

    void unavailablePolicyFailsClosed()
    {
        QFETCH(int, configuration);
        PersistenceAdapterRegistry registry;
        int factoryCalls = 0;
        if (configuration == 1 || configuration == 2) {
            QVERIFY(registry.registerRoute(testSecureDescriptor(), [](const QString &) { return false; }));
        } else if (configuration == 3) {
            QVERIFY(registry.registerAdapter(testSecureDescriptor(), [&]() -> std::unique_ptr<IPersistenceAdapter> {
                ++factoryCalls;
                return {};
            }));
        } else if (configuration >= 4) {
            const qsizetype prefixBytes = configuration == 4 ? 0 : (configuration == 5 ? -1 : 4097);
            QVERIFY(registry.registerRoute(testSecureDescriptor(), [](const QString &) { return false; },
                                           {prefixBytes, &recognizesTestSecure}));
        }
        if (configuration == 2) {
            auto second = testSecureDescriptor();
            second.id = QStringLiteral("test.second-secure");
            QVERIFY(registry.registerRoute(second, [](const QString &) { return false; },
                                           {9, &recognizesTestSecure}));
        }
        const auto policy = registry.readPolicySnapshot();
        QVERIFY(!policy.isAvailable());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("ordinary.txt"));
        const QByteArray bytes("still ordinary\n");
        QVERIFY(writeBytes(path, bytes));
        SharedReadOnlyFile reader;
        QVERIFY(openSharedReadOnlyFile(reader, path));
        const auto denied = openOrdinaryReadSource(reader, path, policy);
        QCOMPARE(denied.status, OrdinaryReadStatus::PolicyUnavailable);
        QVERIFY(!denied.isReady() && !reader.isOpen() && !denied.error.isEmpty());
        const auto absent = openOrdinaryReadSource(reader,
            directory.filePath(QStringLiteral("missing.txt")), policy);
        QCOMPARE(absent.status, OrdinaryReadStatus::PolicyUnavailable);
        QVERIFY(!reader.isOpen());
        QCOMPARE(readBytes(path), bytes);
        QCOMPARE(factoryCalls, 0);
        QCOMPARE(registry.instantiatedCount(), 0);
    }

    void pureSnapshotOutlivesRegistryWithoutFactories()
    {
        auto factoryCalls = std::make_shared<int>(0);
        auto routeCalls = std::make_shared<int>(0);
        bool registrationSucceeded = false;
        const auto policy = [&]() {
            PersistenceAdapterRegistry registry;
            const bool routeRegistered = registry.registerRoute(testSecureDescriptor(),
                [routeCalls](const QString &) { ++*routeCalls; return false; },
                {9, &recognizesTestSecure});
            const bool factoryRegistered = registry.registerAdapter(
                {QStringLiteral("test.plain"), QStringLiteral("Test plain adapter"),
                 0, DocumentPersistence::PlainFile},
                [factoryCalls]() -> std::unique_ptr<IPersistenceAdapter> {
                    ++*factoryCalls;
                    return {};
                });
            registrationSucceeded = routeRegistered && factoryRegistered;
            return registry.readPolicySnapshot();
        }();
        QVERIFY(registrationSucceeded);
        QVERIFY(policy.isAvailable());
        QCOMPARE(policy.requiredPrefixBytes(), qsizetype(9));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString plainPath = directory.filePath(QStringLiteral("ordinary.dat"));
        const QString securePath = directory.filePath(QStringLiteral("custom.dat"));
        const QByteArray plainBytes("ordinary snapshot\n");
        const QByteArray secureBytes("TEST-LOCK custom opaque data\n");
        QVERIFY(writeBytes(plainPath, plainBytes));
        QVERIFY(writeBytes(securePath, secureBytes));
        SharedReadOnlyFile reader;
        const auto plain = openOrdinaryReadSource(reader, plainPath, policy);
        QVERIFY2(plain.isReady(), qPrintable(plain.error));
        QCOMPARE(reader.readAt(0, plainBytes.size()), plainBytes);
        const auto secure = openOrdinaryReadSource(reader, securePath, policy);
        QCOMPARE(secure.status, OrdinaryReadStatus::SecureType);
        QCOMPARE(secure.adapterId, QStringLiteral("test.secure"));
        QVERIFY(!reader.isOpen());
        QCOMPARE(*factoryCalls, 0);
        QCOMPARE(*routeCalls, 0);
        QCOMPARE(readBytes(plainPath), plainBytes);
        QCOMPARE(readBytes(securePath), secureBytes);
    }

    void emptyTextAnalysisStillRequiresAdmission()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("empty.dat"));
        QVERIFY(writeBytes(path, {}));
        AppComposition composition;
        const auto policy = composition.persistenceAdapters().readPolicySnapshot();
        DiskFileStamp stamp;
        QCOMPARE(captureDiskFileStamp(path, &stamp), DiskFileState::Unchanged);
        const auto completed = ReadOnlyTextAnalysis::analyze(
            path, policy, stamp, TextEncoding::Utf8, nullptr, {});
        QCOMPARE(completed.status, ReadOnlyTextAnalysis::Result::Status::Completed);
        QVERIFY(completed.source.isReady());
        QCOMPARE(completed.characterCount, qint64(0));
        QCOMPARE(completed.lineBreakPrefix, QVector<qint64>{0});
        const auto denied = ReadOnlyTextAnalysis::analyze(
            path, {}, stamp, TextEncoding::Utf8, nullptr, {});
        QCOMPARE(denied.status, ReadOnlyTextAnalysis::Result::Status::Failed);
        QCOMPARE(denied.source.status, OrdinaryReadStatus::PolicyUnavailable);
        const auto invalid = ReadOnlyTextAnalysis::analyze(
            path, policy, {}, TextEncoding::Utf8, nullptr, {});
        QCOMPARE(invalid.status, ReadOnlyTextAnalysis::Result::Status::Failed);
        std::atomic_bool canceled{true};
        const auto stopped = ReadOnlyTextAnalysis::analyze(
            path, policy, stamp, TextEncoding::Utf8, &canceled, {});
        QCOMPARE(stopped.status, ReadOnlyTextAnalysis::Result::Status::Canceled);
        QCOMPARE(readBytes(path), QByteArray{});
        QCOMPARE(composition.heavyFactoryCalls(), 0);
    }

    void sameHandleVersionMatchesExistingFingerprint_data()
    {
        QTest::addColumn<int>("size");
        QTest::newRow("small-whole-file-sample") << 8191;
        QTest::newRow("large-three-samples") << 512 * 1024 + 71;
    }

    void sameHandleVersionMatchesExistingFingerprint()
    {
        QFETCH(int, size);
        static_assert(!std::is_convertible_v<DiskFileStamp, DiskFileVersion>,
                      "Read-admission metadata must not masquerade as a save fingerprint.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sampled.dat"));
        QByteArray bytes(size, 'a');
        for (int index = 0; index < size; ++index)
            bytes[index] = char('a' + index % 23);
        QVERIFY(writeBytes(path, bytes));
        AppComposition composition;
        SharedReadOnlyFile reader;
        const auto result = openOrdinaryReadSource(
            reader, path, composition.persistenceAdapters().readPolicySnapshot());
        QVERIFY2(result.isReady(), qPrintable(result.error));
        DiskFileVersion fromHandle;
        DiskFileVersion fromExistingApi;
        QString error;
        QVERIFY2(reader.captureVersion(&fromHandle, &error), qPrintable(error));
        QCOMPARE(captureDiskFileVersion(path, &fromExistingApi), DiskFileState::Unchanged);
        QVERIFY(fromHandle.valid && fromExistingApi.valid && !fromHandle.sampleHash.isEmpty());
        QCOMPARE(fromHandle.absolutePath, fromExistingApi.absolutePath);
        QCOMPARE(fromHandle.size, fromExistingApi.size);
        QCOMPARE(fromHandle.modifiedMs, fromExistingApi.modifiedMs);
        QCOMPARE(fromHandle.fileIdentity, fromExistingApi.fileIdentity);
        QCOMPARE(fromHandle.sampleHash, fromExistingApi.sampleHash);
        QCOMPARE(fromHandle.sampleHash, expectedSampleHash(bytes));

        // Same-identity, same-size, restored-mtime mutation deliberately passes
        // the cheap metadata predicate but must differ under save sampling.
        // This establishes the boundary, not support for adversarial mutation.
        bytes[size / 2] = 'Z';
        {
            QFile writer(path);
            QVERIFY(writer.open(QIODevice::ReadWrite));
            QVERIFY(writer.seek(size / 2));
            QCOMPARE(writer.write("Z", 1), qint64(1));
            QVERIFY(writer.flush());
        }
        QVERIFY(setModifiedTime(path, result.stamp.modifiedMs));
        DiskFileStamp metadataAgain;
        QVERIFY(reader.captureStamp(&metadataAgain, &error));
        QVERIFY(sameDiskFileStamp(result.stamp, metadataAgain));
        DiskFileVersion mutatedHandle;
        DiskFileVersion mutatedPath;
        QVERIFY(reader.captureVersion(&mutatedHandle, &error));
        QCOMPARE(captureDiskFileVersion(path, &mutatedPath), DiskFileState::Unchanged);
        QCOMPARE(mutatedHandle.sampleHash, mutatedPath.sampleHash);
        QCOMPARE(mutatedHandle.sampleHash, expectedSampleHash(bytes));
        QVERIFY(mutatedHandle.sampleHash != fromHandle.sampleHash);
        QCOMPARE(readBytes(path), bytes);
        QCOMPARE(composition.heavyFactoryCalls(), 0);
    }
};

QTEST_GUILESS_MAIN(TestOrdinaryReadSource)
#include "test_ordinary_read_source.moc"
