#pragma once

#include "DocumentSearchSurface.h"
#include "SearchSession.h"

#include <QPointer>
#include <QVector>

// One UI command, not a search engine or scheduler. Its lifetime/active flag
// fences callbacks and editor effects independently of Find All generations.
class SearchNavigationRequest final : public QObject {
    Q_OBJECT
public:
    SearchNavigationRequest(SearchQuery query,
        const QList<DocumentSearchSurface *> &targets,
        DocumentSearchSurface *initialTarget, bool forward, bool wrap,
        QObject *parent);
    ~SearchNavigationRequest() override;
    void start();
    void retire();
    bool active() const { return active_; }
    SearchScope scope() const { return query_.scope; }
    bool accepts(DocumentSearchSurface *target) const;
    // Called only in UI dispatch; legacy QObject callback contexts are kept.
    static bool acceptsContext(QObject *context, DocumentSearchSurface *target);

signals:
    void finished(DocumentSearchSurface *target, bool found, QString error);
    void targetClosed(DocumentSearchSurface *identity);
    void retired();

private:
    struct Attempt {
        QPointer<DocumentSearchSurface> target;
        QUuid documentId;
        bool reset;
    };
    void advance();
    void complete(bool found, QString error);
    SearchQuery query_;
    QVector<Attempt> attempts_;
    QPointer<DocumentSearchSurface> target_;
    QUuid documentId_;
    QMetaObject::Connection destroyedConnection_;
    int next_ = 0;
    bool active_ = true;
    bool started_ = false;
    bool forward_;
    bool wrap_;
};
