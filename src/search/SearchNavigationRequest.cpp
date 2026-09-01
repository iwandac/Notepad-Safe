#include "SearchNavigationRequest.h"

#include <QThread>
#include <QTimer>
#include <utility>
#include <memory>

SearchNavigationRequest::SearchNavigationRequest(
    SearchQuery query, const QList<DocumentSearchSurface *> &targets,
    DocumentSearchSurface *initialTarget, bool forward, bool wrap, QObject *parent)
    : QObject(parent), query_(std::move(query)), forward_(forward), wrap_(wrap)
{
    const auto append = [this](DocumentSearchSurface *target, bool reset) {
        if (target) attempts_.append({target, target->documentState().id, reset});
    };
    if (query_.scope == SearchScope::CurrentDocument) {
        append(initialTarget, false);
        return;
    }
    if (targets.isEmpty()) return;
    const int start = qMax(0, targets.indexOf(initialTarget));
    append(targets.at(start), false);
    const int direction = forward ? 1 : -1;
    for (int i = start + direction; i >= 0 && i < targets.size(); i += direction)
        append(targets.at(i), true);
    if (wrap) {
        for (int i = forward ? 0 : targets.size() - 1; i != start; i += direction)
            append(targets.at(i), true);
        append(targets.at(start), true);
    }
}

SearchNavigationRequest::~SearchNavigationRequest()
{
    retire();
    if (!query_.pattern.isEmpty()) query_.pattern.fill(QChar(0));
}

bool SearchNavigationRequest::accepts(DocumentSearchSurface *target) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return active_ && target_ && target_ == target
        && target_->documentState().id == documentId_;
}

bool SearchNavigationRequest::acceptsContext(
    QObject *context, DocumentSearchSurface *target)
{
    if (!context) return false;
    Q_ASSERT(QThread::currentThread() == context->thread());
    auto *request = qobject_cast<SearchNavigationRequest *>(context);
    if (!request || request->accepts(target)) return true;
    if (request->active_ && target && request->target_ == target)
        request->complete(false, QStringLiteral("文档已变更，请重新查找。"));
    return false;
}

void SearchNavigationRequest::retire()
{
    if (!active_) return;
    active_ = false;
    disconnect(destroyedConnection_);
    emit retired();
}

void SearchNavigationRequest::start()
{
    if (!active_ || started_) return;
    started_ = true;
    advance();
}

void SearchNavigationRequest::complete(bool found, QString error)
{
    if (!active_) return;
    QPointer<SearchNavigationRequest> self(this);
    QPointer<DocumentSearchSurface> target(target_);
    retire();
    if (self) emit finished(target.data(), found, std::move(error));
}

void SearchNavigationRequest::advance()
{
    while (active_ && next_ < attempts_.size()) {
        const Attempt attempt = attempts_.at(next_++);
        auto candidate = attempt.target;
        if (!candidate || candidate->documentState().id != attempt.documentId
            || candidate->isSecureDocumentMasked()
            || candidate->isSecureDocumentTransitioning()
            || (query_.hexBytes && !candidate->isHexPresentation())) continue;
        target_ = candidate;
        documentId_ = attempt.documentId;
        disconnect(destroyedConnection_);
        destroyedConnection_ = connect(candidate, &QObject::destroyed, this,
            [this, identity = candidate.data()] {
                // QWidget emits destroyed before all QPointers are cleared.
                target_.clear();
                QPointer<SearchNavigationRequest> self(this);
                retire();
                if (self) emit targetClosed(identity);
            });
        QPointer<SearchNavigationRequest> self(this);
        if (attempt.reset) candidate->resetSearchPosition(forward_);
        if (!self || !candidate || !acceptsContext(self, candidate)) return;
        // The flag belongs to this attempt, not the command: an inline callback
        // can complete/advance before findNextL2Async returns a rejection.
        auto callbackSeen = std::make_shared<bool>(false);
        // submit can synchronously destroy this command; keep the argument
        // storage alive until the domain method has actually returned.
        const SearchQuery query = query_;
        const bool started = candidate->findNextL2Async(
            query.pattern, query.regex, query.caseSensitive, query.wholeWord,
            forward_, query.scope == SearchScope::CurrentDocument && wrap_,
            query.hexBytes, this,
            [self, candidate, callbackSeen](bool found, QString error) {
                if (*callbackSeen) return;
                *callbackSeen = true;
                if (!self || !candidate || !self->accepts(candidate)) return;
                if (found || self->scope() == SearchScope::CurrentDocument
                    || (!error.isEmpty() && error != QStringLiteral("已取消。"))) {
                    self->complete(found, std::move(error));
                } else {
                    QTimer::singleShot(0, self, [self] {
                        if (self && self->active()) self->advance();
                    });
                }
            });
        if (!self || !candidate || !accepts(candidate) || *callbackSeen) return;
        if (started) return;
        const QString error = candidate->lastSearchError();
        if (!error.isEmpty() || scope() == SearchScope::CurrentDocument) {
            complete(false, error);
            return;
        }
    }
    complete(false, wrap_ ? QStringLiteral("未找到匹配。")
                         : QStringLiteral("已到范围边界。"));
}
