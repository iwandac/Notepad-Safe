#pragma once

#include <QObject>
#include <QString>
#include <functional>

class LargeFileSearch;
class ReadOnlySearchSource;
class VirtualFileView;
class WindowedReadOnlyProvider;

// UI-thread adapter for the two existing source lifetimes. Resources is a
// fresh projection of document/provider facts, never a second policy cache.
class EditorSearchSource final : public QObject
{
    Q_OBJECT
public:
    struct Resources {
        VirtualFileView *mapped;
        WindowedReadOnlyProvider *windowed;
        bool ordinaryReadOnly;
        QString path;
    };
    EditorSearchSource(QObject *owner, std::function<Resources()> access);
    ~EditorSearchSource() override;
    bool ensure(QString *error = nullptr);
    bool isReady() const;
    bool usesWindowedSource() const;
    bool validateCurrent(QString *error = nullptr);
    LargeFileSearch *engine(bool navigation = false) const;
    bool closeAndOwnerAlive();

signals:
    // Search preparation only, not a document reopen or content revision.
    void ready();
    void failed(const QString &message);

private:
    static bool usesWindowed(const Resources &resources);
    std::function<Resources()> m_access;
    ReadOnlySearchSource *m_readOnly = nullptr;
};
