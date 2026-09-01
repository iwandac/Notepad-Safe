#pragma once

#include "SessionDocumentPort.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPushButton;

// A workspace placeholder for one unresolved recovery item. It owns only the
// persisted identity, user-visible diagnosis and explicit actions; plaintext
// and recovery parsing remain outside this view.
class RecoveryIssuePage final : public QWidget, public ISessionDocumentPort {
    Q_OBJECT
public:
    enum class Kind { OriginalFile, UnsavedDraft };

    RecoveryIssuePage(SessionTab state, Kind kind, QString diagnosticCode,
                      QString detail, QWidget *parent = nullptr);

    Kind kind() const { return kind_; }
    void updateFailure(const QString &diagnosticCode, const QString &detail);
    QString diagnosticCode() const { return diagnosticCode_; }

    SessionTab captureSessionTab() const override { return state_; }
    QUuid sessionInstanceId() const override { return instanceId_; }
    quint64 sessionRevision() const override { return 0; }
    bool needsRecoverySnapshot() const override { return false; }
    bool isEmptyUntitledDocument() const override { return false; }
    bool isLargeRecoveryDocument() const override { return false; }
    QString recoveryPath() const override { return state_.autosavePath; }
    void acceptRecoveryPath(const QString &path) override {
        state_.autosavePath = path;
    }
    bool captureRecoveryPayload(SessionRecoveryPayload *, QString *) override {
        return false;
    }
    bool restoreSessionRecovery(const SessionRestoreRequest &, QString *) override {
        return false;
    }

signals:
    void retryRequested();
    void discardConfirmed();
    void originalRemovalRequested();

private:
    SessionTab state_;
    Kind kind_;
    QUuid instanceId_ = QUuid::createUuid();
    QString diagnosticCode_;
    QLabel *causeLabel_ = nullptr;
    QLabel *codeLabel_ = nullptr;
    QLabel *detailLabel_ = nullptr;
    QPushButton *detailsToggle_ = nullptr;
    QPushButton *discardButton_ = nullptr;
    QPushButton *confirmDiscardButton_ = nullptr;
    QPushButton *cancelDiscardButton_ = nullptr;
};
