#pragma once

#include "SessionDocumentPort.h"
#include "RecoveryIssuePage.h"

#include <QPointer>
#include <QVector>

class EditorTab;
class MainWindow;

// Owns the post-first-paint restore queue and unresolved-item lifecycle.
// SessionManager remains the only parser and managed-file deletion authority.
class WorkspaceRecoveryCoordinator final {
public:
    explicit WorkspaceRecoveryCoordinator(MainWindow *window);
    static WorkspaceRecoveryCoordinator &forWindow(MainWindow *window);

    void start();
    void restoreNext();
    QVector<SessionCaptureEntry> captureEntries(int *activeIndex);
    bool mayCreateTarget(int itemIndex) const;
    void attachInFlightTarget(int itemIndex, EditorTab *tab);
    bool beginWindowClose();
    void cancelWindowClose();
    bool isInFlight(const QWidget *widget) const;
    void noteAcceptedRemoval(QWidget *widget);
    bool prepareSessionSave(bool allowPreservation, bool *preserveInvalidSession);
    void reportSessionSaveOutcome(bool failed, const QString &error,
                                  const QString &preservedPath);

private:
    enum class Phase { Pending, InFlight, Live, RetainedFailure, Closed };
    struct Item {
        SessionTab original;
        Phase phase = Phase::Pending;
        QPointer<QWidget> widget;
    };
    void ensureOriginalRecordsLoaded();
    void queueNext();
    void beginFlight(int index);
    void endFlight();
    void placeDeliveredTab(int itemIndex);
    int itemForWidget(const QWidget *widget) const;
    void finish();
    RecoveryIssuePage *replaceWithIssue(EditorTab *failedTab,
                                        const SessionTab &state,
                                        RecoveryIssuePage::Kind kind,
                                        const QString &diagnosticCode,
                                        const QString &detail);
    EditorTab *openOriginalFile(const SessionTab &state, bool *reused,
                               QString *diagnosticCode, QString *detail);
    bool restoreDraft(EditorTab *tab, const SessionTab &state,
                      QString *diagnosticCode, QString *detail);
    void retry(RecoveryIssuePage *issue);
    void discard(RecoveryIssuePage *issue);
    void removeOriginalIssue(RecoveryIssuePage *issue);
    void removeIssuePage(RecoveryIssuePage *issue);
    void removeEditorWithoutRecoveryAction(EditorTab *tab);

    MainWindow *window_ = nullptr;
    QVector<Item> items_;
    QVector<int> restoreOrder_;
    QPointer<EditorTab> inFlightTarget_;
    QPointer<QWidget> inFlightOriginalPage_;
    int inFlightIndex_ = -1;
    Phase phaseBeforeFlight_ = Phase::Pending;
    quint64 scheduleEpoch_ = 0;
    int activeTab_ = 0;
    int processedCount_ = 0;
    bool hasUnsaved_ = false;
    bool recordsLoaded_ = false;
    bool started_ = false;
    bool finished_ = false;
    bool closing_ = false;
    bool deferredClose_ = false;
};
