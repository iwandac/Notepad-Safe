#include "SingleFindAllRequest.h"
#include "DocumentSearchSurface.h"
#include "SearchResultsModel.h"
#include "SearchSession.h"
#include "EncodedRegexCompiler.h"
#include "Pcre2Regex.h"

bool validateFindAllQuery(const SearchQuery &query, DocumentSearchSurface *target,
                         QString *validationError)
{
    const bool single = query.scope == SearchScope::CurrentDocument;
    if (single && (target->isSecureDocumentMasked() || target->isSecureDocumentTransitioning())) {
        *validationError = QStringLiteral("当前安全文档已锁定，无法搜索正文。");
        return false;
    }
    QByteArray bytePattern;
    // The caller accepted a nonempty literal; it never enters PCRE2.
    bool valid = true;
    if (query.hexBytes) {
        valid = LargeFileSearch::parseHexBytePattern(
            query.pattern, &bytePattern, validationError);
    } else if (query.regex && single
               && target->isL2Mode() && !target->isHexPresentation()
               && !EncodedRegexCompiler::isUtf8SubjectEncoding(
                   target->displayEncoding())) {
        EncodedRegexCompiler::Output translated;
        valid = EncodedRegexCompiler::compile(
            query.pattern, target->displayEncoding(), query.caseSensitive,
            &translated, validationError);
    } else if (query.regex) {
        Pcre2Regex validator;
        valid = validator.compile(query.pattern, false,
                                  query.caseSensitive, query.wholeWord, validationError);
    }
    bytePattern.fill('\0');
    if (valid && query.hexBytes && single && !target->isHexPresentation()) {
        *validationError = QStringLiteral("字节搜索仅适用于十六进制视图；请切换视图后重试。");
        return false;
    }
    return valid;
}

struct SingleFindAllRequest::ModelLease {
    QPointer<QObject> arena;
    QPointer<SearchResultsModel> model;
    QMetaObject::Connection terminalConnection;
    QString pattern;
    QString fileName;
    bool submissionComplete = false;
    bool accepted = false;
    bool terminalSeen = false;
    bool retired = false;
};

SingleFindAllRequest::SingleFindAllRequest(DocumentSearchSurface *target,
                                           int generation, QObject *parent)
    : QObject(parent), target_(target), documentId_(target->documentState().id),
      generation_(generation)
{
    connect(target, &QObject::destroyed, this, [this] {
        // QWidget emits destroyed before all observers' QPointers are cleared.
        target_.clear();
        retire();
        emit targetClosed();
    });
}

SingleFindAllRequest::~SingleFindAllRequest()
{
    retire(); // No borrowed SearchSession access, including during parent teardown.
}

bool SingleFindAllRequest::current(const SearchSession &session,
                                    DocumentSearchSurface *activeTarget, bool requireActive) const
{
    return (!requireActive || (!retired_ && session.pendingTargets() > 0))
        && session.isCurrent(generation_) && target_ && target_ == activeTarget
        && target_->documentState().id == documentId_;
}

void SingleFindAllRequest::collectModelArena(const std::shared_ptr<ModelLease> &lease)
{
    if (!lease || !lease->arena || !lease->retired
        || (!lease->terminalSeen && !(lease->submissionComplete && !lease->accepted)))
        return;
    QObject::disconnect(lease->terminalConnection);
    // Adopted models have been reparented out; only unclaimed children remain.
    lease->arena->deleteLater();
}

void SingleFindAllRequest::retire()
{
    retired_ = true;
    if (target_)
        disconnect(target_, nullptr, this, nullptr);
    if (lease_) {
        lease_->retired = true;
        collectModelArena(lease_);
    }
}

bool SingleFindAllRequest::startL2(const SearchQuery &query)
{
    if (retired_ || !target_ || lease_)
        return false;
    lease_ = std::make_shared<ModelLease>();
    const auto lease = lease_;
    // The domain captures this raw parent. Its lifetime follows the SOURCE,
    // never the short-lived request; retirement cannot invalidate that pointer.
    lease->arena = new QObject(target_);
    lease->terminalConnection = connect(target_, &DocumentSearchSurface::findAllL2Finished,
        lease->arena, [lease](SearchResultsModel *model, const QString &pattern, const QString &name) {
            if (lease->terminalSeen || (model && model->QObject::parent() != lease->arena))
                return;
            lease->terminalSeen = true;
            lease->model = model;
            lease->pattern = pattern;
            lease->fileName = name;
            collectModelArena(lease);
        });
    connect(target_, &DocumentSearchSurface::findAllL2Progress, this,
        [this](int percent, qint64 done, qint64 total) {
            if (!retired_ && deliveryEnabled_)
                emit progress(percent, done, total);
        });
    connect(target_, &DocumentSearchSurface::findAllL2Partial, this,
        [this](const QVector<LargeFileSearchMatch> &batch, const QString &, const QString &,
               TextEncoding encoding, bool byteMode) {
            if (!retired_ && deliveryEnabled_)
                emit partial(batch, encoding, byteMode);
        });
    connect(target_, &DocumentSearchSurface::findAllL2Finished, this,
        [this](SearchResultsModel *, const QString &, const QString &) { deliverTerminal(); });
    const QPointer<SingleFindAllRequest> owner(this);
    const bool accepted = target_->findAllL2Async(query.pattern, query.regex,
        query.caseSensitive, query.wholeWord, lease->arena, query.hexBytes);
    lease->accepted = accepted;
    lease->submissionComplete = true;
    if (owner && !accepted)
        owner->retire();
    collectModelArena(lease);
    return accepted;
}

void SingleFindAllRequest::enableDelivery()
{
    deliveryEnabled_ = true;
    deliverTerminal();
}

void SingleFindAllRequest::deliverTerminal()
{
    if (retired_ || delivered_ || !deliveryEnabled_ || !lease_ || !lease_->terminalSeen)
        return;
    delivered_ = true;
    const auto lease = lease_;
    emit completed(lease->model, lease->pattern, lease->fileName);
    // The receiver adopts before displaying. If it became stale during the
    // handoff, no ownership transfer occurred and the arena reclaims the model.
    lease->retired = true;
    collectModelArena(lease);
}
