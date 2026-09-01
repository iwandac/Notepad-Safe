#include <QApplication>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QTest>
#include <Qsci/qsciscintilla.h>

#include <utility>

#include "AppComposition.h"
#include "AsyncTaskManager.h"
#include "DiskFileVersion.h"
#include "DocumentOpenWorkflow.h"
#include "EditorTab.h"
#include "WindowedReadOnlyProvider.h"

namespace {
constexpr qint64 InitialSize = 33LL * 1024 * 1024;

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    return digest.addData(&file) ? digest.result() : QByteArray{};
}

bool terminal(const AsyncTask &task)
{
    return task.id > 0 && task.state != AsyncTaskState::Queued && task.state != AsyncTaskState::Running;
}

class ModalGuard final : public QObject {
public:
    ModalGuard() { qApp->installEventFilter(this); }
    ~ModalGuard() override { qApp->removeEventFilter(this); }
    QStringList shown;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            if (auto *dialog = qobject_cast<QDialog *>(watched); dialog && dialog->isModal()) {
                shown.append(dialog->windowTitle());
                const QPointer<QDialog> guard(dialog);
                QTimer::singleShot(0, this, [guard] { if (guard) guard->reject(); });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

// The existing drop-surface signal exposes newly created editors before the
// provider opens its source. Observe real textChanged, including transient
// content that a later rejection might clear; do not infer safety from only
// the final empty page or a guessed decoding of encrypted source bytes.
class ProjectionLog final : public QObject {
public:
    explicit ProjectionLog(EditorTab *tab)
    {
        observe(tab->editor());
        connect(tab, &EditorTab::externalDropSurfaceCreated, this,
                [this](QWidget *surface) {
                    observe(surface);
                    for (auto *editor : surface->findChildren<QsciScintilla *>()) observe(editor);
                    for (auto *editor : surface->findChildren<QPlainTextEdit *>()) observe(editor);
                }, Qt::DirectConnection);
    }
    QStringList texts;
    int observedEditors() const { return observed_.size(); }
private:
    void observe(QWidget *surface)
    {
        if (!surface || observed_.contains(surface)) return;
        if (auto *editor = qobject_cast<QsciScintilla *>(surface)) {
            observed_.insert(editor);
            texts.append(editor->text());
            connect(editor, &QsciScintilla::textChanged, this,
                    [this, editor] { texts.append(editor->text()); }, Qt::DirectConnection);
        } else if (auto *editor = qobject_cast<QPlainTextEdit *>(surface)) {
            observed_.insert(editor);
            texts.append(editor->toPlainText());
            connect(editor, &QPlainTextEdit::textChanged, this,
                    [this, editor] { texts.append(editor->toPlainText()); }, Qt::DirectConnection);
        }
    }
    QSet<QObject *> observed_;
};

// An actual filesystem race at the public surface boundary, not a provider
// replacement or a fabricated open result. The real workflow/provider already
// selected its route; the unchanged request is then handled by the real tab.
class ReplaceBeforeSurface final : public IDocumentSurface {
public:
    ReplaceBeforeSurface(EditorTab &tab, AppComposition &composition, QByteArray replacement)
        : tab_(tab), composition_(composition), replacement_(std::move(replacement)) {}

    bool openSurface(const DocumentSurfaceOpenRequest &request, QString *error) override
    {
        ++calls;
        received = request;
        factoriesAtBoundary = composition_.heavyFactoryCalls();
        persistenceInstancesAtBoundary = composition_.persistenceAdapters().instantiatedCount();
        if (calls != 1) {
            failure = QStringLiteral("Workflow called the surface more than once");
            if (error) *error = failure;
            return false;
        }
        QSaveFile replacement(request.path);
        replacement.setDirectWriteFallback(false);
        committed = replacement.open(QIODevice::WriteOnly)
            && replacement.write(replacement_) == replacement_.size() && replacement.commit();
        if (!committed) {
            failure = replacement.errorString();
            if (error) *error = failure;
            return false;
        }
        delegated = true;
        actualResult = tab_.openSurface(request, &failure);
        if (error) *error = failure;
        return actualResult;
    }

    int calls = 0;
    int factoriesAtBoundary = -1;
    int persistenceInstancesAtBoundary = -1;
    DocumentSurfaceOpenRequest received;
    bool committed = false;
    bool delegated = false;
    bool actualResult = false;
    QString failure;
private:
    EditorTab &tab_;
    AppComposition &composition_;
    QByteArray replacement_;
};
} // namespace

class SmokeReadAdmissionOpen : public QObject {
    Q_OBJECT
private slots:
    void initialReadOnlyRouteSurvivesAtomicReplacement_data()
    {
        QTest::addColumn<bool>("secureReplacement");
        QTest::newRow("ordinarySmallReplacement") << false;
        QTest::newRow("actualSecureSmallReplacement") << true;
    }

    void initialReadOnlyRouteSurvivesAtomicReplacement()
    {
        QFETCH(bool, secureReplacement);
        const QString fixturePath = QFINDTESTDATA("../fixtures/kdbx/Format400.kdbx");
        QFile fixture(fixturePath);
        QVERIFY2(fixture.open(QIODevice::ReadOnly), qPrintable(fixture.errorString()));
        const QByteArray secureBytes = fixture.readAll();
        fixture.close();
        const QByteArray secureHash = QByteArray::fromHex(
            "4f23b3f5f6c71209e135dab92d0f06034315d3f70e3e2ef3ec3a77ff37b49435");
        QCOMPARE(secureBytes.size(), qsizetype(1801));
        QCOMPARE(secureBytes.first(12).toHex(), QByteArray("03d9a29a67fb4bb500000400"));
        QCOMPARE(QCryptographicHash::hash(secureBytes, QCryptographicHash::Sha256), secureHash);
        const QByteArray ordinaryBytes("ordinary small replacement remains read-only\nsecond line\n");
        const QByteArray replacement = secureReplacement ? secureBytes : ordinaryBytes;
        const QByteArray replacementHash = QCryptographicHash::hash(replacement, QCryptographicHash::Sha256);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString profile = directory.filePath(QStringLiteral("profile"));
        QVERIFY(QDir().mkpath(profile));
        const bool hadProfile = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        const QByteArray previousProfile = qgetenv("NOTEPADSAFE_DATADIR");
        QVERIFY(qputenv("NOTEPADSAFE_DATADIR", profile.toUtf8()));
        const auto restoreProfile = qScopeGuard([&] {
            if (hadProfile) qputenv("NOTEPADSAFE_DATADIR", previousProfile);
            else qunsetenv("NOTEPADSAFE_DATADIR");
        });
        const QString path = directory.filePath(QStringLiteral("route-race.dat"));
        {
            QFile initial(path);
            QVERIFY(initial.open(QIODevice::WriteOnly));
            const QByteArray chunk = (QByteArray(127, 'a') + '\n').repeated(8192);
            for (int index = 0; index < 33; ++index) QCOMPARE(initial.write(chunk), qint64(chunk.size()));
            QCOMPARE(initial.size(), InitialSize);
        }
        DiskFileVersion originalVersion;
        QCOMPARE(captureDiskFileVersion(path, &originalVersion), DiskFileState::Unchanged);
        QVERIFY(originalVersion.valid && !originalVersion.fileIdentity.isEmpty());

        AppComposition composition;
        QVERIFY(composition.persistenceAdapters().readPolicySnapshot().isAvailable());
        QVERIFY(!composition.persistenceAdapters().adapter(QStringLiteral("secure.kdbx")));
        ModalGuard modalGuard;
        EditorTab tab;
        tab.resize(1000, 650);
        tab.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tab));
        auto *initialEditor = tab.editor();
        QVERIFY(initialEditor && initialEditor->text().isEmpty() && !tab.isModified());
        const DocumentState initialState = tab.documentState();
        const bool initialEditorReadOnly = initialEditor->isReadOnly();
        const bool initialEditorModified = initialEditor->isModified();
        ProjectionLog projections(&tab);
        auto &manager = AsyncTaskManager::instance();
        QVector<qint64> ownedTasks;
        QVector<qint64> securityTasks;
        QObject taskObservation;
        connect(&manager, &AsyncTaskManager::taskAdded, &taskObservation, [&](qint64 id) {
            ownedTasks.append(id);
            const auto type = manager.taskById(id).type;
            if (type == AsyncTaskType::SecureOpen || type == AsyncTaskType::SecureSave
                || type == AsyncTaskType::SecureIdentity || type == AsyncTaskType::VaultOperation)
                securityTasks.append(id);
        }, Qt::DirectConnection);
        const auto settled = [&] {
            for (qint64 id : std::as_const(ownedTasks)) if (!terminal(manager.taskById(id))) return false;
            return true;
        };
        const auto cleanup = qScopeGuard([&] {
            for (qint64 id : std::as_const(ownedTasks)) manager.cancel(id);
            if (!QTest::qWaitFor(settled, 3000)) qWarning("Read-admission owned task drain exceeded 3 seconds");
            tab.shutdownL2BeforeClose();
        });
        ReplaceBeforeSurface surface(tab, composition, replacement);
        const bool opened = openDocumentByPolicy(composition.documentProviders(), &surface, path);
        QVERIFY2(surface.calls == 1 && surface.committed && surface.delegated, qPrintable(surface.failure));
        QVERIFY(QTest::qWaitFor(settled, 3000));
        // Drain posted GUI deliveries; no sleep is used to create the race.
        bool deliveryTail = false;
        QObject fence;
        QMetaObject::invokeMethod(&fence, [&] { deliveryTail = true; }, Qt::QueuedConnection);
        QVERIFY(QTest::qWaitFor([&] { return deliveryTail; }, 1000));
        DiskFileVersion replacementVersion;
        QCOMPARE(captureDiskFileVersion(path, &replacementVersion), DiskFileState::Unchanged);
        QVERIFY(replacementVersion.valid && replacementVersion.fileIdentity != originalVersion.fileIdentity);
        QCOMPARE(replacementVersion.size, qint64(replacement.size()));
        QCOMPARE(fileHash(path), replacementHash);
        QCOMPARE(fileHash(fixturePath), secureHash);
        qInfo() << "INITIAL_READ_ADMISSION" << QTest::currentDataTag()
                << "windowed=" << surface.received.windowed << "requireReadOnly=" << surface.received.requireReadOnly
                << "policyAvailable=" << surface.received.readPolicy.isAvailable()
                << "opened=" << opened << "error=" << surface.failure
                << "factoriesBefore=" << surface.factoriesAtBoundary << "factoriesAfter=" << composition.heavyFactoryCalls()
                << "securityTasks=" << securityTasks.size() << "observedEditors=" << projections.observedEditors();
        for (qint64 id : std::as_const(securityTasks)) {
            const AsyncTask task = manager.taskById(id);
            qInfo() << "INITIAL_READ_ADMISSION_SECURITY_TASK" << id << int(task.type)
                    << task.categoryTag << task.title;
        }
        QCOMPARE(composition.heavyFactoryCalls(), surface.factoriesAtBoundary);
        QCOMPARE(composition.persistenceAdapters().instantiatedCount(), surface.persistenceInstancesAtBoundary);
        QVERIFY(!composition.persistenceAdapters().adapter(QStringLiteral("secure.kdbx")));
        QVERIFY(securityTasks.isEmpty());
        QVERIFY(modalGuard.shown.isEmpty() && !QApplication::activeModalWidget());
        QVERIFY(!QFileInfo::exists(QDir(profile).filePath(QStringLiteral("security-session-v1.bin"))));
        QCOMPARE(surface.received.path, path);
        QVERIFY(surface.received.windowed && surface.received.requireReadOnly);
        QCOMPARE(surface.received.contentKind, DocumentContentKind::Text);
        QCOMPARE(surface.received.presentation, DocumentPresentationKind::Text);
        QVERIFY(surface.received.readPolicy.isAvailable());
        QCOMPARE(surface.received.readPolicy.secureAdapterFor(path, QByteArrayView(secureBytes).first(12)),
                 QStringLiteral("secure.kdbx"));
        QCOMPARE(opened, surface.actualResult);

        if (secureReplacement) {
            const auto actualRoute = composition.persistenceAdapters().resolve(path);
            QVERIFY(actualRoute.has_value());
            QCOMPARE(actualRoute->id, QStringLiteral("secure.kdbx"));
            QCOMPARE(actualRoute->persistence, DocumentPersistence::SecureFile);
            QVERIFY2(!opened, "Initial read-only route accepted the new small secure source as ordinary content");
            QVERIFY(!surface.failure.isEmpty());
            for (const QString &text : std::as_const(projections.texts))
                QVERIFY2(text.isEmpty(), "A real editor briefly projected content from the rejected secure source");
            const DocumentState after = tab.documentState();
            QCOMPARE(after.id, initialState.id);
            QCOMPARE(after.revision, initialState.revision);
            QCOMPARE(after.location, initialState.location);
            QCOMPARE(after.origin, initialState.origin);
            QCOMPARE(after.persistence, initialState.persistence);
            QCOMPARE(after.contentKind, initialState.contentKind);
            QCOMPARE(after.securityClass, initialState.securityClass);
            QCOMPARE(after.capabilities, initialState.capabilities);
            QCOMPARE(after.dirty, initialState.dirty);
            QCOMPARE(tab.editor(), initialEditor);
            QCOMPARE(tab.editor()->isReadOnly(), initialEditorReadOnly);
            QCOMPARE(tab.editor()->isModified(), initialEditorModified);
            QVERIFY(tab.editor()->text().isEmpty() && !tab.isModified() && !tab.isSecureDocument());
            if (auto *provider = tab.windowedReadOnlyProvider()) {
                // hasSource tracks the requested path; accepted evidence is
                // the full version, which must remain invalid on rejection.
                QVERIFY(!provider->sourceVersion().valid);
                QVERIFY(provider->sourceBytes(0, 32).isEmpty());
            }
            // The original blank editor may remain editable. Rejecting an
            // open must not be mistaken for promoting the rejected source.
        } else {
            QVERIFY2(opened, qPrintable(surface.failure));
            auto *provider = tab.windowedReadOnlyProvider();
            QVERIFY(provider && tab.isReadOnlyByPolicy() && tab.editor() == provider->editor());
            QVERIFY(tab.editor()->isVisible() && tab.editor()->isReadOnly());
            QCOMPARE(provider->fileSize(), qint64(ordinaryBytes.size()));
            QCOMPARE(provider->sourceBytes(0, ordinaryBytes.size()), ordinaryBytes);
            QCOMPARE(tab.editor()->text(), QString::fromUtf8(ordinaryBytes));
            const DocumentState after = tab.documentState();
            QCOMPARE(after.location, QFileInfo(path).absoluteFilePath());
            QVERIFY(!after.capabilities.testFlag(DocumentCapability::Edit)
                    && !after.capabilities.testFlag(DocumentCapability::Save));
            QVERIFY(!tab.isModified() && !after.dirty && !tab.isSecureDocument());
            QVERIFY(projections.observedEditors() >= 2 && projections.texts.contains(QString::fromUtf8(ordinaryBytes)));
        }
        QCOMPARE(fileHash(path), replacementHash);
        QCOMPARE(fileHash(fixturePath), secureHash);
        QCOMPARE(composition.heavyFactoryCalls(), surface.factoriesAtBoundary);
        QVERIFY(securityTasks.isEmpty() && modalGuard.shown.isEmpty());
    }
};

QTEST_MAIN(SmokeReadAdmissionOpen)
#include "smoke_read_admission_open.moc"
