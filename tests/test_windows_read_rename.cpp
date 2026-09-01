#include <QCryptographicHash>
#include <QFile>
#include <QMap>
#include <QTemporaryDir>
#include <QtTest>

#include "OrdinaryReadSource.h"
#include "SharedReadOnlyFile.h"

#include <cstddef>
#include <cstring>
#include <memory>

namespace {

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QByteArray hash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QString stampText(const DiskFileStamp &stamp)
{
    return QStringLiteral("valid=%1 size=%2 mtime=%3 identity=%4")
        .arg(stamp.valid).arg(stamp.size).arg(stamp.modifiedMs)
        .arg(QString::fromLatin1(stamp.fileIdentity));
}

struct Outcome {
    bool called = false;
    bool succeeded = false;
    unsigned long error = 0;
    QString actualTarget;
};

#ifdef Q_OS_WIN
class NativeHandle {
public:
    explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) : value(value) {}
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
    HANDLE value;
};

HANDLE openNative(const QString &path, DWORD access)
{
    return ::CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
}

QByteArray readNative(HANDLE reader, DWORD count, DWORD *error)
{
    LARGE_INTEGER start{};
    if (!::SetFilePointerEx(reader, start, nullptr, FILE_BEGIN)) {
        *error = ::GetLastError();
        return {};
    }
    QByteArray bytes(count, Qt::Uninitialized);
    DWORD received = 0;
    if (!::ReadFile(reader, bytes.data(), count, &received, nullptr)) {
        *error = ::GetLastError();
        return {};
    }
    *error = ERROR_SUCCESS;
    bytes.resize(received);
    return bytes;
}

bool stampNative(HANDLE reader, DiskFileStamp *stamp, DWORD *error)
{
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(reader, &info)) {
        *error = ::GetLastError();
        return false;
    }
    *error = ERROR_SUCCESS;
    stamp->valid = true;
    stamp->size = qint64((quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    const quint64 time = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32)
        | info.ftLastWriteTime.dwLowDateTime;
    stamp->modifiedMs = qint64(time / 10000) - 11644473600000LL;
    stamp->fileIdentity = diskFileIdentity(info);
    return true;
}

Outcome renamePosix(const QString &replacement, const QString &target)
{
    NativeHandle source(openNative(replacement, GENERIC_READ | DELETE));
    if (source.value == INVALID_HANDLE_VALUE)
        return {false, false, ::GetLastError()};
#if defined(FILE_RENAME_FLAG_POSIX_SEMANTICS) && NTDDI_VERSION >= NTDDI_WIN10_RS1
    const DWORD nameBytes = DWORD(target.size() * sizeof(wchar_t));
    const size_t bufferBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
    // Operator new supplies native structure alignment; FileName is a counted
    // absolute UTF-16 name. Retain a zeroed trailing wchar outside its count.
    const std::unique_ptr<void, void (*)(void *)> buffer(
        ::operator new(bufferBytes), [](void *p) { ::operator delete(p); });
    std::memset(buffer.get(), 0, bufferBytes);
    auto *info = static_cast<FILE_RENAME_INFO *>(buffer.get());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->FileNameLength = nameBytes;
    std::memcpy(info->FileName, target.utf16(), nameBytes);
    ::SetLastError(ERROR_SUCCESS);
    const BOOL renamed = ::SetFileInformationByHandle(source.value, FileRenameInfoEx,
                                                      info, DWORD(bufferBytes));
    const DWORD error = ::GetLastError();
    wchar_t finalPath[32768]{};
    const DWORD pathLength = ::GetFinalPathNameByHandleW(source.value, finalPath, 32768, FILE_NAME_NORMALIZED);
    const QString actualTarget = pathLength > 0 && pathLength < 32768
        ? QString::fromWCharArray(finalPath, pathLength) : QStringLiteral("unavailable");
    return {true, renamed != FALSE, error, actualTarget};
#else
    Q_UNUSED(target);
    return {false, false, ERROR_NOT_SUPPORTED};
#endif
    // Only the replacement handle closes here. The old target reader remains
    // alive in the caller through all postconditions, including old-byte reads.
}
#endif

} // namespace

class TestWindowsReadRename : public QObject {
    Q_OBJECT
    QMap<QString, Outcome> m_outcomes;

private slots:
    void nativeRename_data()
    {
        QTest::addColumn<QString>("primitive");
        QTest::addColumn<QString>("readerKind");
        QTest::newRow("move-shared") << QStringLiteral("MoveFileExW") << QStringLiteral("shared");
        QTest::newRow("move-raw") << QStringLiteral("MoveFileExW") << QStringLiteral("raw");
        QTest::newRow("move-none") << QStringLiteral("MoveFileExW") << QStringLiteral("none");
        QTest::newRow("posix-shared") << QStringLiteral("FileRenameInfoEx") << QStringLiteral("shared");
        QTest::newRow("posix-raw") << QStringLiteral("FileRenameInfoEx") << QStringLiteral("raw");
    }

    void nativeRename()
    {
#ifndef Q_OS_WIN
        QSKIP("This diagnostic exercises native Windows rename primitives.");
#else
        QFETCH(QString, primitive);
        QFETCH(QString, readerKind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString target = QDir::toNativeSeparators(directory.filePath(QStringLiteral("old.dat")));
        const QString replacement = QDir::toNativeSeparators(directory.filePath(QStringLiteral("new.dat")));
        const QByteArray original(8192, 'A');
        const QByteArray updated(8192, 'B');
        QVERIFY(writeBytes(target, original));
        QVERIFY(writeBytes(replacement, updated));
        QCOMPARE(hash(readBytes(target)), hash(original));
        QCOMPARE(hash(readBytes(replacement)), hash(updated));
        DiskFileStamp before, replacementBefore;
        QString error;
        QCOMPARE(captureDiskFileStamp(target, &before, &error), DiskFileState::Unchanged);
        QCOMPARE(captureDiskFileStamp(replacement, &replacementBefore, &error), DiskFileState::Unchanged);
        QVERIFY(before.valid && replacementBefore.valid);
        QVERIFY(before.fileIdentity != replacementBefore.fileIdentity);

        SharedReadOnlyFile shared;
        NativeHandle raw;
        if (readerKind == QLatin1String("shared")) {
            QVERIFY2(openSharedReadOnlyFile(shared, target, &error), qPrintable(error));
            DiskFileStamp held;
            QVERIFY(shared.captureStamp(&held, &error));
            QVERIFY(sameDiskFileStamp(held, before));
            QCOMPARE(shared.readAt(0, original.size(), &error), original);
        } else if (readerKind == QLatin1String("raw")) {
            raw.value = openNative(target, GENERIC_READ);
            const DWORD openError = ::GetLastError();
            QVERIFY2(raw.value != INVALID_HANDLE_VALUE, qPrintable(QString::number(openError)));
            DiskFileStamp held;
            DWORD nativeError = 0;
            QVERIFY(stampNative(raw.value, &held, &nativeError));
            QVERIFY(sameDiskFileStamp(held, before));
            QCOMPARE(readNative(raw.value, DWORD(original.size()), &nativeError), original);
            QCOMPARE(nativeError, DWORD(ERROR_SUCCESS));
        }

        Outcome outcome;
        if (primitive == QLatin1String("MoveFileExW")) {
            ::SetLastError(ERROR_SUCCESS);
            const BOOL renamed = ::MoveFileExW(reinterpret_cast<LPCWSTR>(replacement.utf16()),
                reinterpret_cast<LPCWSTR>(target.utf16()), MOVEFILE_REPLACE_EXISTING);
            const DWORD nativeError = ::GetLastError();
            outcome = {true, renamed != FALSE, nativeError};
        } else {
            outcome = renamePosix(replacement, target);
        }
        m_outcomes.insert(QString::fromLatin1(QTest::currentDataTag()), outcome);
        qInfo().noquote() << "NATIVE_RENAME" << QTest::currentDataTag()
            << "primitive=" << primitive << "reader=" << readerKind
            << "called=" << outcome.called << "BOOL=" << outcome.succeeded
            << "GetLastError=" << outcome.error << "actual-target=" << outcome.actualTarget
            << "expected-target=" << QStringLiteral("\\\\?\\") + target << "before{" << stampText(before)
            << "} replacement{" << stampText(replacementBefore) << '}';

        DiskFileStamp after;
        QCOMPARE(captureDiskFileStamp(target, &after, &error), DiskFileState::Unchanged);
        qInfo().noquote() << "NATIVE_RENAME_AFTER" << QTest::currentDataTag()
            << stampText(after) << "sha256=" << hash(readBytes(target));
        if (outcome.succeeded) {
            if (primitive == QLatin1String("FileRenameInfoEx"))
                QCOMPARE(outcome.actualTarget, QStringLiteral("\\\\?\\") + target);
            QCOMPARE(after.fileIdentity, replacementBefore.fileIdentity);
            QVERIFY(after.fileIdentity != before.fileIdentity);
            QCOMPARE(after.size, qint64(updated.size()));
            QCOMPARE(hash(readBytes(target)), hash(updated));
            QVERIFY(!QFile::exists(replacement));
        } else {
            QVERIFY(sameDiskFileStamp(after, before));
            QCOMPARE(hash(readBytes(target)), hash(original));
            DiskFileStamp replacementAfter;
            QCOMPARE(captureDiskFileStamp(replacement, &replacementAfter, &error), DiskFileState::Unchanged);
            QVERIFY(sameDiskFileStamp(replacementAfter, replacementBefore));
            QCOMPARE(hash(readBytes(replacement)), hash(updated));
        }
        // No close/reopen or pathname substitution is permitted for this proof.
        if (readerKind == QLatin1String("shared")) {
            QVERIFY(shared.isOpen());
            QCOMPARE(shared.readAt(0, original.size(), &error), original);
            DiskFileStamp held;
            QVERIFY(shared.captureStamp(&held, &error));
            QVERIFY(sameDiskFileStamp(held, before));
            qInfo().noquote() << "OLD_READER_STILL_OPEN shared" << stampText(held);
        } else if (readerKind == QLatin1String("raw")) {
            DWORD nativeError = 0;
            QCOMPARE(readNative(raw.value, DWORD(original.size()), &nativeError), original);
            QCOMPARE(nativeError, DWORD(ERROR_SUCCESS));
            DiskFileStamp held;
            QVERIFY(stampNative(raw.value, &held, &nativeError));
            QVERIFY(sameDiskFileStamp(held, before));
            qInfo().noquote() << "OLD_READER_STILL_OPEN raw" << stampText(held);
        }
        if (!outcome.succeeded)
            qInfo() << "Replacement capability NOT demonstrated in this row; failure integrity checks passed.";
#endif
    }

    void cleanupTestCase()
    {
        for (const auto &prefix : {QStringLiteral("move"), QStringLiteral("posix")}) {
            const auto shared = m_outcomes.constFind(prefix + QStringLiteral("-shared"));
            const auto raw = m_outcomes.constFind(prefix + QStringLiteral("-raw"));
            if (shared == m_outcomes.cend() || raw == m_outcomes.cend()) continue;
            qInfo().noquote() << "RAW_VS_SHARED" << prefix
                << "sameOutcome=" << (shared->called == raw->called
                    && shared->succeeded == raw->succeeded
                    && (shared->succeeded || shared->error == raw->error))
                << "shared(called,BOOL,error)=" << shared->called << shared->succeeded << shared->error
                << "raw(called,BOOL,error)=" << raw->called << raw->succeeded << raw->error;
        }
        const auto none = m_outcomes.constFind(QStringLiteral("move-none"));
        if (none != m_outcomes.cend())
            qInfo() << "MOVE_NO_OLD_HANDLE" << "BOOL=" << none->succeeded << "error=" << none->error;
    }
};

QTEST_GUILESS_MAIN(TestWindowsReadRename)
#include "test_windows_read_rename.moc"
