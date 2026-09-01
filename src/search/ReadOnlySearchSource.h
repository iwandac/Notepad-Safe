#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include "OrdinaryReadSource.h"

class LargeFileSearch;

// Ordinary read-only documents borrow one mapping for two search lanes, not
// a hidden view. Completed indexes outlive that mapping; display bytes remain
// the responsibility of the document's bounded read-only provider.
class ReadOnlySearchSource final : public QObject
{
    Q_OBJECT
public:
    explicit ReadOnlySearchSource(QObject *parent = nullptr);
    ~ReadOnlySearchSource() override;
    bool ensureReady(const QString &path, const PersistenceReadPolicy &policy,
                     const DiskFileStamp &expected, QString *error = nullptr);
    bool isReady() const;
    qint64 preparationTaskId() const;
    LargeFileSearch *findAllEngine() const { return m_findAll; }
    LargeFileSearch *navigationEngine() const { return m_navigation; }
    void close();

signals:
    void ready();
    void failed(const QString &message);

private:
    struct Region;
    struct Preparation;
    static void settlePreparation(const std::shared_ptr<Preparation> &job);
    void installPreparation(const std::shared_ptr<Preparation> &job);
    void releaseMappingIfIdle();
    LargeFileSearch *m_findAll;
    LargeFileSearch *m_navigation;
    std::unique_ptr<Region> m_region;
    std::shared_ptr<Preparation> m_preparation;
    bool m_closing = false; // admission only; never substitutes for a reader barrier
};
