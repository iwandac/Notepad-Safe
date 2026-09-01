#pragma once

#include "LargeFileSearch.h"
#include <QObject>
#include <QPointer>
#include <QUuid>
#include <memory>

class DocumentSearchSurface;
class SearchResultsModel;
class SearchSession;
struct SearchQuery;

bool validateFindAllQuery(const SearchQuery &query, DocumentSearchSurface *target,
                         QString *validationError);

// One UI-facing submission/connection lifetime; SearchSession owns query/generation.
class SingleFindAllRequest final : public QObject {
    Q_OBJECT
public:
    SingleFindAllRequest(DocumentSearchSurface *target, int generation, QObject *parent);
    ~SingleFindAllRequest() override;
    bool current(const SearchSession &session, DocumentSearchSurface *activeTarget,
                 bool requireActive = true) const;
    int generation() const { return generation_; }
    QUuid documentId() const { return documentId_; }
    bool startL2(const SearchQuery &query);
    void enableDelivery();
    void retire();

signals:
    void progress(int percent, qint64 done, qint64 total);
    void partial(const QVector<LargeFileSearchMatch> &batch,
                 TextEncoding encoding, bool bytePositionMode);
    void completed(SearchResultsModel *model, const QString &pattern, const QString &fileName);
    void targetClosed();

private:
    struct ModelLease;
    static void collectModelArena(const std::shared_ptr<ModelLease> &lease);
    void deliverTerminal();
    QPointer<DocumentSearchSurface> target_;
    QUuid documentId_;
    int generation_;
    bool retired_ = false;
    bool deliveryEnabled_ = false;
    bool delivered_ = false;
    std::shared_ptr<ModelLease> lease_;
};
