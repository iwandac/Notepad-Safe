#pragma once

#include <QFile>
#include <QIODevice>
#include <QByteArray>
#include <QString>

#include "DiskFileVersion.h"

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// QFile::map() cannot reliably map a QFile opened from an externally supplied
// CRT descriptor on Windows. Opening a plain QFile, on the other hand, keeps a
// long-lived mmap viewer from observing atomic-save replacements. This small
// QFile-shaped owner keeps the sharing contract and the mapping implementation
// together. It deliberately exposes only the operations used by the two large
// file providers.
class SharedReadOnlyFile final
{
public:
    SharedReadOnlyFile() = default;
    ~SharedReadOnlyFile() { close(); }

    SharedReadOnlyFile(const SharedReadOnlyFile &) = delete;
    SharedReadOnlyFile &operator=(const SharedReadOnlyFile &) = delete;

    void setFileName(const QString &path) { m_path = path; }
    QString fileName() const { return m_path; }

    bool open(QIODevice::OpenMode mode = QIODevice::ReadOnly)
    {
        close();
        m_openFailureState = DiskFileState::Unreadable;
        if (!(mode & QIODevice::ReadOnly) || m_path.isEmpty()) {
            m_error = QStringLiteral("A non-empty read-only path is required");
            return false;
        }
#ifdef Q_OS_WIN
        m_fileHandle = ::CreateFileW(
            reinterpret_cast<LPCWSTR>(m_path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (m_fileHandle == INVALID_HANDLE_VALUE) {
            const DWORD code = ::GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                m_openFailureState = DiskFileState::Missing;
            setWindowsError(QStringLiteral("CreateFile"));
            return false;
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(m_fileHandle, &size)) {
            setWindowsError(QStringLiteral("GetFileSizeEx"));
            close();
            return false;
        }
        m_size = size.QuadPart;
        m_openFailureState = DiskFileState::Unchanged;
        m_error.clear();
        return true;
#else
        m_file.setFileName(m_path);
        if (!m_file.open(mode)) {
            m_error = m_file.errorString();
            // Error classification only, never an admission check. Successful
            // reads below always use the descriptor already owned by m_file.
            struct stat pathInfo{};
            if (::stat(QFile::encodeName(m_path).constData(), &pathInfo) != 0
                && (errno == ENOENT || errno == ENOTDIR))
                m_openFailureState = DiskFileState::Missing;
            return false;
        }
        m_size = m_file.size();
        m_openFailureState = DiskFileState::Unchanged;
        m_error.clear();
        return true;
#endif
    }

    bool isOpen() const
    {
#ifdef Q_OS_WIN
        return m_fileHandle != INVALID_HANDLE_VALUE;
#else
        return m_file.isOpen();
#endif
    }

    qint64 size() const { return m_size; }
    QString errorString() const { return m_error; }
    DiskFileState openFailureState() const { return m_openFailureState; }

    // Read attributes from this handle, not from the path which may now name
    // another file. No payload or bounded fingerprint samples are read here.
    bool captureStamp(DiskFileStamp *stamp, QString *error = nullptr) const
    {
        if (stamp) *stamp = {};
        if (error) error->clear();
        if (!isOpen()) {
            if (error) *error = QStringLiteral("Source handle is not open");
            return false;
        }
        DiskFileStamp captured;
#ifdef Q_OS_WIN
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(m_fileHandle, &info)) {
            const DWORD code = ::GetLastError();
            if (error) *error = QStringLiteral("GetFileInformationByHandle failed with Windows error %1")
                                    .arg(static_cast<qulonglong>(code));
            return false;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (error) *error = QStringLiteral("Source handle is not a regular file");
            return false;
        }
        const quint64 bytes = (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (bytes > quint64(std::numeric_limits<qint64>::max())) {
            if (error) *error = QStringLiteral("Source size exceeds the supported range");
            return false;
        }
        captured.size = static_cast<qint64>(bytes);
        const quint64 modified = (quint64(info.ftLastWriteTime.dwHighDateTime) << 32)
                                 | info.ftLastWriteTime.dwLowDateTime;
        captured.modifiedMs = static_cast<qint64>(modified / 10000) - 11644473600000LL;
        captured.fileIdentity = diskFileIdentity(info);
#else
        struct stat info{};
        if (::fstat(m_file.handle(), &info) != 0) {
            if (error) *error = QStringLiteral("fstat failed: %1")
                                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        if (!S_ISREG(info.st_mode) || info.st_size < 0) {
            if (error) *error = QStringLiteral("Source handle is not a regular file");
            return false;
        }
        captured.size = static_cast<qint64>(info.st_size);
#  ifdef Q_OS_DARWIN
        captured.modifiedMs = qint64(info.st_mtimespec.tv_sec) * 1000
                              + info.st_mtimespec.tv_nsec / 1000000;
#  else
        captured.modifiedMs = qint64(info.st_mtim.tv_sec) * 1000
                              + info.st_mtim.tv_nsec / 1000000;
#  endif
        captured.fileIdentity = diskFileIdentity(info);
#endif
        captured.valid = true;
        if (stamp) *stamp = std::move(captured);
        return true;
    }

    // Full save-conflict evidence, only after the caller has admitted this
    // handle. It retains the existing <=192 KiB algorithm and rejects drift.
    bool captureVersion(DiskFileVersion *version, QString *error = nullptr) const
    {
        if (version) *version = {};
        if (error) error->clear();
        DiskFileStamp before;
        if (!captureStamp(&before, error)) return false;
        if (before.size != m_size) {
            if (error) *error = QStringLiteral("Opened source size changed before fingerprinting");
            return false;
        }
        DiskFileVersion captured;
        captured.absolutePath = QFileInfo(m_path).absoluteFilePath();
        captured.size = before.size;
        captured.modifiedMs = before.modifiedMs;
        captured.fileIdentity = before.fileIdentity;
        QString readError;
        if (!captureDiskFileSampleHash(before.size,
                [this, &readError](qint64 offset, qint64 count) {
                    return readAt(offset, static_cast<qsizetype>(count), &readError);
                }, &captured.sampleHash)) {
            if (error) *error = readError.isEmpty()
                ? QStringLiteral("Short read while fingerprinting opened source") : readError;
            return false;
        }
        DiskFileStamp after;
        if (!captureStamp(&after, error)) return false;
        if (!sameDiskFileStamp(before, after)) {
            if (error) *error = QStringLiteral("Opened source changed while fingerprinting");
            return false;
        }
        captured.valid = true;
        if (version) *version = std::move(captured);
        return true;
    }

    QByteArray readAt(qint64 offset, qsizetype requested, QString *error = nullptr) const
    {
        if (error) error->clear();
        if (!isOpen() || offset < 0 || requested <= 0 || offset >= m_size)
            return {};
        const quint64 available = static_cast<quint64>(m_size - offset);
        const quint64 wanted = std::min<quint64>(static_cast<quint64>(requested), available);
        QByteArray result;
        result.resize(static_cast<qsizetype>(wanted));
#ifdef Q_OS_WIN
        quint64 completed = 0;
        while (completed < wanted) {
            const DWORD piece = static_cast<DWORD>(std::min<quint64>(
                wanted - completed, std::numeric_limits<DWORD>::max()));
            const quint64 absolute = static_cast<quint64>(offset) + completed;
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(absolute);
            if (!::SetFilePointerEx(m_fileHandle, position, nullptr, FILE_BEGIN)) {
                if (error)
                    *error = QStringLiteral("SetFilePointerEx failed with Windows error %1")
                                 .arg(static_cast<qulonglong>(::GetLastError()));
                return {};
            }
            DWORD received = 0;
            const BOOL ok = ::ReadFile(
                m_fileHandle, result.data() + static_cast<qsizetype>(completed),
                piece, &received, nullptr);
            const DWORD nativeError = ok ? ERROR_SUCCESS : ::GetLastError();
            if (!ok) {
                if (error) {
                    *error = QStringLiteral("ReadFile failed with Windows error %1")
                                 .arg(static_cast<qulonglong>(nativeError));
                }
                return {};
            }
            if (received == 0)
                break;
            completed += received;
        }
        result.resize(static_cast<qsizetype>(completed));
#else
        quint64 completed = 0;
        while (completed < wanted) {
            const size_t piece = static_cast<size_t>(std::min<quint64>(
                wanted - completed, std::numeric_limits<ssize_t>::max()));
            const quint64 absolute = static_cast<quint64>(offset) + completed;
            if (absolute > static_cast<quint64>(std::numeric_limits<off_t>::max())) {
                if (error) *error = QStringLiteral("Source offset exceeds the native file range");
                return {};
            }
            const ssize_t received = ::pread(m_file.handle(),
                result.data() + static_cast<qsizetype>(completed), piece,
                static_cast<off_t>(absolute));
            if (received < 0) {
                if (errno == EINTR) continue;
                if (error) *error = QStringLiteral("pread failed: %1")
                                        .arg(QString::fromLocal8Bit(std::strerror(errno)));
                return {};
            }
            if (received == 0) break;
            completed += static_cast<quint64>(received);
        }
        result.resize(static_cast<qsizetype>(completed));
#endif
        return result;
    }

    uchar *map(qint64 offset, qint64 length)
    {
        if (!isOpen() || offset < 0 || length <= 0 || offset > m_size
            || length > m_size - offset) {
            m_error = QStringLiteral("Invalid mapping range");
            return nullptr;
        }
        if (m_view) {
            m_error = QStringLiteral("Only one active mapping is supported");
            return nullptr;
        }
#ifdef Q_OS_WIN
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        const qint64 granularity = static_cast<qint64>(info.dwAllocationGranularity);
        const qint64 alignedOffset = (offset / granularity) * granularity;
        const qint64 delta = offset - alignedOffset;
        const quint64 mappingLength = static_cast<quint64>(length)
                                      + static_cast<quint64>(delta);
        if (mappingLength > static_cast<quint64>(std::numeric_limits<SIZE_T>::max())) {
            m_error = QStringLiteral("Mapping range exceeds the process address space");
            return nullptr;
        }
        m_mappingHandle = ::CreateFileMappingW(
            m_fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!m_mappingHandle) {
            setWindowsError(QStringLiteral("CreateFileMapping"));
            return nullptr;
        }
        const quint64 nativeOffset = static_cast<quint64>(alignedOffset);
        m_viewBase = ::MapViewOfFile(
            m_mappingHandle, FILE_MAP_READ,
            static_cast<DWORD>(nativeOffset >> 32),
            static_cast<DWORD>(nativeOffset & 0xffffffffULL),
            static_cast<SIZE_T>(mappingLength));
        if (!m_viewBase) {
            setWindowsError(QStringLiteral("MapViewOfFile"));
            ::CloseHandle(m_mappingHandle);
            m_mappingHandle = nullptr;
            return nullptr;
        }
        m_view = static_cast<uchar *>(m_viewBase) + delta;
#else
        m_view = m_file.map(offset, length);
        m_viewBase = m_view;
        if (!m_view) {
            m_error = m_file.errorString();
            return nullptr;
        }
#endif
        m_error.clear();
        return m_view;
    }

    bool unmap(uchar *address)
    {
        if (!m_view || address != m_view)
            return false;
#ifdef Q_OS_WIN
        const bool ok = ::UnmapViewOfFile(m_viewBase) != FALSE;
        if (!ok)
            setWindowsError(QStringLiteral("UnmapViewOfFile"));
        m_view = nullptr;
        m_viewBase = nullptr;
        if (m_mappingHandle) {
            ::CloseHandle(m_mappingHandle);
            m_mappingHandle = nullptr;
        }
        return ok;
#else
        const bool ok = m_file.unmap(address);
        if (!ok)
            m_error = m_file.errorString();
        m_view = nullptr;
        m_viewBase = nullptr;
        return ok;
#endif
    }

    void close()
    {
        if (m_view)
            unmap(m_view);
#ifdef Q_OS_WIN
        if (m_mappingHandle) {
            ::CloseHandle(m_mappingHandle);
            m_mappingHandle = nullptr;
        }
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_fileHandle);
            m_fileHandle = INVALID_HANDLE_VALUE;
        }
#else
        m_file.close();
#endif
        m_size = 0;
    }

private:
#ifdef Q_OS_WIN
    void setWindowsError(const QString &operation)
    {
        m_error = QStringLiteral("%1 failed with Windows error %2")
                      .arg(operation)
                      .arg(static_cast<qulonglong>(::GetLastError()));
    }

    HANDLE m_fileHandle = INVALID_HANDLE_VALUE;
    HANDLE m_mappingHandle = nullptr;
#else
    QFile m_file;
#endif
    QString m_path;
    QString m_error;
    DiskFileState m_openFailureState = DiskFileState::NotTracked;
    qint64 m_size = 0;
    uchar *m_view = nullptr;
    void *m_viewBase = nullptr;
};

inline bool openSharedReadOnlyFile(SharedReadOnlyFile &file, const QString &path,
                                   QString *error = nullptr)
{
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
