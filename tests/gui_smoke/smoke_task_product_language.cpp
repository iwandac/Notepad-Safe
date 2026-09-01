#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtGui/QAccessible>
#include <QtGui/QAccessibleInterface>
#include <QtGui/QAction>
#include <QtGui/QPaintDevice>
#include <QtGui/QPaintEngine>
#include <QtGui/QPalette>
#include <QtGui/QPainter>
#include <QtGui/QTextCursor>
#include <QtTest/QAbstractItemModelTester>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QAbstractItemDelegate>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QStyleOptionViewItem>
#include <QtWidgets/QToolButton>

#include <memory>

#include "AsyncTaskManager.h"
#include "AsyncTaskPanelDialog.h"
#include "MainWindow.h"
#include "StatusBarTaskWidget.h"

// 2B3 groups 2/3 only. Register with the existing GUI smoke target helper.
// MainWindow is used only as a hidden host for the existing appearance actions
// in opt-in visual runs. No file recovery/search or production testing hooks.
// New product object names agreed for this acceptance slice:
// backgroundTaskSummary, backgroundTaskDetailsToggle,
// backgroundTaskTechnicalDetails. Existing list/details/thread names survive.
namespace {

const QString kTaskTitle = QStringLiteral("任务语言验收");
const QString kRawMessage = QStringLiteral("RAW_TASK_ERROR QFile::map mmap overlay failed");
const QString kSource = QStringLiteral("task_language_probe.cpp");
const QString kStack = QStringLiteral("frame[0] = 0xABCDEF_PATCH_PROBE");
const QString kDescription = QStringLiteral("RAW_DESCRIPTION Scintilla Provider Adapter");
constexpr qint64 kBatchId = 887766;

bool nativeEvidenceMode()
{
    return qgetenv("NOTEPADSAFE_TASK_STYLE") == QByteArrayLiteral("native");
}

QString evidenceModeTag()
{
    const QString theme = QString::fromUtf8(qgetenv("NOTEPADSAFE_TASK_THEME"));
    return (nativeEvidenceMode() ? QStringLiteral("native-") : QStringLiteral("fusion-"))
        + (theme.isEmpty() ? QStringLiteral("platform") : theme);
}

void sizePanelForMode(AsyncTaskPanelDialog &panel)
{
    panel.resize(nativeEvidenceMode() ? QSize(860, 580) : QSize(1180, 680));
}

AsyncErrorInfo diagnosticFailure(const QString &userMessage = {})
{
    AsyncErrorInfo error;
    error.message = kRawMessage;
    error.sourceFile = kSource;
    error.sourceLine = 714;
    error.stackFrames = {kStack};
    error.userMessage = userMessage;
    return error;
}

struct GatedTask {
    std::shared_ptr<QSemaphore> gate = std::make_shared<QSemaphore>();
    qint64 id = 0;

    explicit GatedTask(const QString &userMessage = {}, const QString &description = kDescription)
    {
        id = AsyncTaskManager::instance().submit(
            AsyncTaskType::Other, AsyncTaskPriority::High, kTaskTitle,
            description,
            [barrier = gate, userMessage](qint64, AsyncCancelPtr cancel,
                            const std::function<void(qint64, qint64)> &,
                            const std::function<void(const AsyncErrorInfo &)> &reportError) {
                while (!(cancel && cancel->load(std::memory_order_acquire))) {
                    if (barrier->tryAcquire(1, 20)) {
                        if (!(cancel && cancel->load(std::memory_order_acquire)))
                            reportError(diagnosticFailure(userMessage));
                        return;
                    }
                }
            }, kBatchId, QStringLiteral("TASK_LANGUAGE_CATEGORY"));
    }

    ~GatedTask()
    {
        // QtTest early-return assertions must not strand a worker at the gate.
        // An assertion may return while the test has disabled cancellation.
        AsyncTaskManager::instance().setTaskCancellable(id, true);
        AsyncTaskManager::instance().cancel(id);
        gate->release();
    }

    // The CMake Qt-case registrar scans zero-argument "void name()" lines;
    // keep this non-slot helper outside that pattern.
    auto fail() const -> void { gate->release(); }
};

QModelIndex indexForTask(QListView *list, qint64 id)
{
    if (!list || !list->model()) return {};
    for (int row = 0; row < list->model()->rowCount(); ++row) {
        const QModelIndex candidate = list->model()->index(row, 0);
        if (candidate.data(AsyncTaskManager::TaskIdRole).toLongLong() == id)
            return candidate;
    }
    return {};
}

QString widgetText(QWidget *widget)
{
    if (auto *label = qobject_cast<QLabel *>(widget)) return label->text();
    if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) return edit->toPlainText();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) return button->text();
    return {};
}

QString selectedSummary(AsyncTaskPanelDialog &panel)
{
    if (QWidget *summary = panel.findChild<QWidget *>(QStringLiteral("backgroundTaskSummary")))
        return summary->isVisible() ? widgetText(summary) : QString();
    // Before the fix, this is the selected task's user-visible details pane.
    // This fallback makes the live-update red test fail on stale content, not
    // merely because the proposed new summary object does not yet exist.
    return widgetText(panel.findChild<QPlainTextEdit *>(QStringLiteral("backgroundTaskDetails")));
}

QString accessibleText(QAccessibleInterface *interface)
{
    if (!interface || !interface->isValid()) return {};
    return interface->text(QAccessible::Name) + QLatin1Char('\n')
        + interface->text(QAccessible::Description) + QLatin1Char('\n')
        + interface->text(QAccessible::Value);
}

QString visibleAccessibleText(QAccessibleInterface *interface, int depth = 0)
{
    if (!interface || !interface->isValid() || depth > 12
        || interface->state().invisible || interface->state().offscreen) return {};
    QString out = accessibleText(interface);
    for (int child = 0; child < qMin(interface->childCount(), 256); ++child)
        out += QLatin1Char('\n') + visibleAccessibleText(interface->child(child), depth + 1);
    return out;
}

QAccessibleInterface *taskItemInterface(QAccessibleInterface *interface, int depth = 0)
{
    if (!interface || !interface->isValid() || depth > 8) return nullptr;
    if (interface->role() == QAccessible::ListItem
        && accessibleText(interface).contains(kTaskTitle)) return interface;
    for (int child = 0; child < qMin(interface->childCount(), 256); ++child) {
        if (auto *found = taskItemInterface(interface->child(child), depth + 1)) return found;
    }
    return nullptr;
}

// Minimal text-only paint sink, not a replacement delegate or OCR oracle.
// The installed production delegate and QStyle still issue every paint call.
class TextPaintEngine final : public QPaintEngine {
public:
    TextPaintEngine() : QPaintEngine(QPaintEngine::AllFeatures) {}
    QStringList text;
    QPointF previousBaseline;
    qreal previousRight = 0;
    bool begin(QPaintDevice *) override { setActive(true); return true; }
    bool end() override { setActive(false); return true; }
    Type type() const override { return QPaintEngine::User; }
    void updateState(const QPaintEngineState &) override {}
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
    void drawPath(const QPainterPath &) override {}
    void drawPolygon(const QPointF *, int, PolygonDrawMode) override {}
    void drawRects(const QRectF *, int) override {}
    void drawTextItem(const QPointF &baseline, const QTextItem &item) override
    {
        // Font/script shaping may split one literal line into several items.
        // Join only adjacent runs, not a new line or a repeated style paint.
        const bool continuesLine = !text.isEmpty()
            && qAbs(baseline.y() - previousBaseline.y()) <= 0.5
            && qAbs(baseline.x() - previousRight) <= 1.0;
        if (continuesLine) text.last() += item.text();
        else text.append(item.text());
        previousBaseline = baseline;
        previousRight = baseline.x() + item.width();
    }
};

class TextPaintDevice final : public QPaintDevice {
public:
    mutable TextPaintEngine engine;
    QPaintEngine *paintEngine() const override { return &engine; }
protected:
    int metric(PaintDeviceMetric metric) const override
    {
        switch (metric) {
        case PdmWidth: return 1600;
        case PdmHeight: return 300;
        case PdmWidthMM: return 423;
        case PdmHeightMM: return 79;
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDepth: return 32;
        case PdmNumColors: return 0;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(QPaintDevice::devicePixelRatioFScale());
        default: return QPaintDevice::metric(metric);
        }
    }
};

QString paintedRow(QListView *list, const QModelIndex &index)
{
    if (!list || !index.isValid()) return {};
    TextPaintDevice device;
    QStyleOptionViewItem option;
    option.initFrom(list);
    option.font = list->font();
    option.fontMetrics = QFontMetrics(option.font);
    option.state |= QStyle::State_Selected;
    option.rect = QRect(0, 0, list->viewport()->width(),
        qMax(120, list->itemDelegate()->sizeHint(option, index).height()));
    QPainter painter(&device);
    list->itemDelegate()->paint(&painter, option, index);
    painter.end();
    return device.engine.text.join(QLatin1Char('\n'));
}

QString defaultSurfaceText(AsyncTaskPanelDialog &panel, QListView *list, qint64 id)
{
    QString out = paintedRow(list, indexForTask(list, id));
    for (QWidget *widget : panel.findChildren<QWidget *>()) {
        if (!widget->isVisible()) continue;
        out += QLatin1Char('\n') + widgetText(widget) + QLatin1Char('\n')
            + widget->toolTip() + QLatin1Char('\n') + widget->accessibleName()
            + QLatin1Char('\n') + widget->accessibleDescription();
    }
    return out + QLatin1Char('\n')
        + visibleAccessibleText(QAccessible::queryAccessibleInterface(&panel));
}

QString diagnosticsFound(const QString &text)
{
    QStringList found;
    // These are controlled product/diagnostic fields, not user filenames or
    // arbitrary document content. Standard OVR/INS is intentionally absent.
    const QStringList probes = {QStringLiteral("RAW_TASK_ERROR"), kSource, kStack,
        QStringLiteral("QFile::map"), QStringLiteral("mmap"), QStringLiteral("overlay"),
        QStringLiteral("Scintilla"), QStringLiteral("Provider"), QStringLiteral("Adapter"),
        QStringLiteral("RAW_DESCRIPTION"), QStringLiteral("TASK_LANGUAGE_CATEGORY"),
        QString::number(kBatchId), QStringLiteral("任务ID"), QStringLiteral("批次ID"),
        QStringLiteral("线程"), QStringLiteral("优先级"), QStringLiteral("高(用户操作)")};
    for (const QString &probe : probes) {
        if (text.contains(probe)) found.append(probe);
    }
    return found.join(QStringLiteral(", "));
}

bool saveEvidence(AsyncTaskPanelDialog &panel, const QString &name)
{
    // A text change can precede its posted LayoutRequest. Do not mistake that
    // transient frame for a complete screenshot or fix clipping by resizing.
    auto *summary = panel.findChild<QLabel *>(QStringLiteral("backgroundTaskSummary"));
    QElapsedTimer settling;
    settling.start();
    QRect previousGeometry;
    QSize previousSize;
    int stablePasses = 0;
    while (settling.elapsed() < 1500 && stablePasses < 3) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(10);
        const QRect geometry = summary ? summary->geometry() : QRect();
        if (geometry == previousGeometry && panel.size() == previousSize) ++stablePasses;
        else stablePasses = 0;
        previousGeometry = geometry;
        previousSize = panel.size();
    }
    const int requiredHeight = summary ? (summary->hasHeightForWidth()
        ? summary->heightForWidth(summary->width()) : summary->sizeHint().height()) : -1;
    const bool summaryFits = summary && summary->isVisible() && requiredHeight >= 0
        && summary->height() >= requiredHeight
        && panel.rect().contains(QRect(summary->mapTo(&panel, QPoint()), summary->size()));
    const QString directory = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("task-product-language-evidence/") + evidenceModeTag());
    if (!QDir().mkpath(directory)) return false;
    const QString path = QDir(directory).filePath(name + QStringLiteral(".png"));
    qInfo().noquote() << "2B3 task panel screenshot (manual review required):" << path
        << "panel=" << panel.size() << "summary=" << (summary ? summary->geometry() : QRect())
        << "requiredHeight=" << requiredHeight << "stable=" << (stablePasses >= 3);
    const bool saved = panel.grab().save(path); // Preserve evidence even if geometry fails.
    if (!summaryFits) qWarning().noquote() << "Task user summary is clipped or unavailable:" << path;
    return saved && stablePasses >= 3 && summaryFits;
}

} // namespace

class TaskProductLanguageSmoke final : public QObject {
    Q_OBJECT
private:
    QTemporaryDir profile_;
    QByteArray oldDataDir_;
    bool hadDataDir_ = false;
    std::unique_ptr<MainWindow> themeHost_;
    QPalette oldPalette_;
    QString oldStyleSheet_;

private slots:
    void initTestCase()
    {
        QVERIFY(profile_.isValid());
        hadDataDir_ = qEnvironmentVariableIsSet("NOTEPADSAFE_DATADIR");
        oldDataDir_ = qgetenv("NOTEPADSAFE_DATADIR");
        qputenv("NOTEPADSAFE_DATADIR", profile_.path().toUtf8());
        const QByteArray style = qgetenv("NOTEPADSAFE_TASK_STYLE");
        const QByteArray theme = qgetenv("NOTEPADSAFE_TASK_THEME");
        QVERIFY2(style.isEmpty() || style == "fusion" || style == "native",
            "NOTEPADSAFE_TASK_STYLE must be fusion or native");
        QVERIFY2(theme.isEmpty() || theme == "light" || theme == "dark",
            "NOTEPADSAFE_TASK_THEME must be light or dark");
        const QString startupStyle = QApplication::style()->objectName();
        oldPalette_ = QApplication::palette();
        oldStyleSheet_ = qApp->styleSheet();
        // Native styles may rasterize an entire control before it reaches a
        // paint engine. Fusion makes the text sink deterministic in this test
        // process; real QWidget screenshots remain a separate visual oracle.
        if (!nativeEvidenceMode()) {
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        } else {
            QVERIFY2(QGuiApplication::platformName() == QStringLiteral("windows"),
                "Native evidence must run on the Windows platform, not offscreen/minimal");
            QVERIFY2(!startupStyle.contains(QStringLiteral("fusion"), Qt::CaseInsensitive),
                "Native evidence must retain the startup Windows style, not a Fusion override");
            QVERIFY2(!theme.isEmpty(), "Native evidence requires explicit light or dark");
            qInfo("NATIVE VISUAL EVIDENCE MODE: Fusion-specific paint-sink assertions are not run;"
                  " model, summary, accessibility, cancellation and geometry assertions remain active.");
        }
        if (!theme.isEmpty()) {
            themeHost_ = std::make_unique<MainWindow>();
            auto *appearance = themeHost_->findChild<QAction *>(
                QStringLiteral("view.appearance.") + QString::fromUtf8(theme));
            QVERIFY(appearance);
            appearance->trigger();
            QVERIFY(appearance->isChecked());
            QApplication::processEvents(QEventLoop::AllEvents, 50);
            QSettings saved(QDir(profile_.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
            QCOMPARE(saved.value(QStringLiteral("appearanceMode")).toString(), QString::fromUtf8(theme));
            const int baseLightness = QApplication::palette().color(QPalette::Base).lightness();
            QVERIFY2(theme == "dark" ? baseLightness < 100 : baseLightness > 180,
                "The real product appearance action did not establish the requested palette");
        }
        qInfo().noquote() << "2B3 capture mode=" << evidenceModeTag()
            << "platform=" << QGuiApplication::platformName() << "startupStyle=" << startupStyle
            << "effectiveStyle=" << QApplication::style()->metaObject()->className()
            << "base=" << QApplication::palette().color(QPalette::Base).name()
            << "highlight=" << QApplication::palette().color(QPalette::Highlight).name()
            << "highlightedText=" << QApplication::palette().color(QPalette::HighlightedText).name();
        QAccessible::setActive(true);
    }

    void init()
    {
        auto &manager = AsyncTaskManager::instance();
        manager.cancelAllByType();
        QTRY_COMPARE_WITH_TIMEOUT(manager.cancellableCount(), 0, 3000);
        manager.clearHistory();
        QCOMPARE(manager.rowCount(), 0);
    }

    void cleanup()
    {
        auto &manager = AsyncTaskManager::instance();
        manager.cancelAllByType();
        QTRY_COMPARE_WITH_TIMEOUT(manager.cancellableCount(), 0, 3000);
        manager.clearHistory();
    }

    void cleanupTestCase()
    {
        themeHost_.reset(); // Still uses the isolated profile during destruction.
        QApplication::setPalette(oldPalette_);
        qApp->setStyleSheet(oldStyleSheet_);
        if (hadDataDir_) qputenv("NOTEPADSAFE_DATADIR", oldDataDir_);
        else qunsetenv("NOTEPADSAFE_DATADIR");
    }

    void tc_2b3_task_selected_summary_tracks_phase_and_terminal_reset()
    {
        auto &manager = AsyncTaskManager::instance();
        QAbstractItemModelTester modelTester(&manager,
            QAbstractItemModelTester::FailureReportingMode::Fatal, this);
        GatedTask task;
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Running, 3000);
        AsyncTaskPanelDialog panel;
        sizePanelForMode(panel);
        panel.show();
        panel.raise();
        panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        QVERIFY(list);
        manager.updateTaskPhase(task.id, QStringLiteral("读取内容"), 1, 2, 1234, 10000);
        list->setCurrentIndex(indexForTask(list, task.id)); // Exactly one selection by the test.
        QTRY_VERIFY_WITH_TIMEOUT(selectedSummary(panel).contains(QStringLiteral("12.34%")), 1000);
        QCOMPARE(manager.taskById(task.id).percentHundredths(), 1234);
        QVERIFY(saveEvidence(panel, QStringLiteral("selected-phase-one")));

        manager.updateTaskPhase(task.id, QStringLiteral("整理结果"), 2, 2, 1, 3);
        QCOMPARE(manager.taskById(task.id).percentHundredths(), 3333);
        QVERIFY(manager.taskById(task.id).completedPhases.contains(QStringLiteral("读取内容")));
        QTRY_VERIFY_WITH_TIMEOUT(selectedSummary(panel).contains(QStringLiteral("33.33%"))
            && selectedSummary(panel).contains(QStringLiteral("整理结果")), 1500);
        const QString painted = paintedRow(list, indexForTask(list, task.id));
        if (!nativeEvidenceMode()) {
            QVERIFY2(painted.contains(kTaskTitle) && painted.contains(QStringLiteral("33.33%"))
                && painted.contains(QStringLiteral("整理结果")), qPrintable(painted));
        }
        auto *item = taskItemInterface(QAccessible::queryAccessibleInterface(list));
        QVERIFY(item);
        const QString spoken = accessibleText(item);
        QVERIFY2(spoken.contains(QStringLiteral("33.33%"))
            && spoken.contains(QStringLiteral("整理结果")), qPrintable(spoken));
        const QString accessibleRole = indexForTask(list, task.id).data(Qt::AccessibleTextRole).toString();
        QVERIFY2(accessibleRole.contains(QStringLiteral("33.33%"))
            && accessibleRole.contains(QStringLiteral("整理结果")), qPrintable(accessibleRole));
        QVERIFY(saveEvidence(panel, QStringLiteral("selected-phase-two")));

        QSignalSpy reset(&manager, &QAbstractItemModel::modelReset);
        task.fail();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Failed, 3000);
        QVERIFY(reset.count() > 0); // Exercise the real active -> history move.
        QTRY_COMPARE_WITH_TIMEOUT(list->currentIndex().data(AsyncTaskManager::TaskIdRole).toLongLong(),
            task.id, 1500);
        QTRY_VERIFY_WITH_TIMEOUT(selectedSummary(panel).contains(QStringLiteral("失败"))
            || selectedSummary(panel).contains(QStringLiteral("未完成")), 1500);
        QVERIFY2(!selectedSummary(panel).contains(QStringLiteral("运行中")), qPrintable(selectedSummary(panel)));
        const QString failedPaint = paintedRow(list, indexForTask(list, task.id));
        if (!nativeEvidenceMode()) {
            QVERIFY2(failedPaint.contains(QStringLiteral("失败"))
                || failedPaint.contains(QStringLiteral("未完成")), qPrintable(failedPaint));
        }
        item = taskItemInterface(QAccessible::queryAccessibleInterface(list));
        QVERIFY(item);
        QVERIFY2(accessibleText(item).contains(QStringLiteral("失败"))
            || accessibleText(item).contains(QStringLiteral("未完成")), qPrintable(accessibleText(item)));
        QCOMPARE(diagnosticsFound(defaultSurfaceText(panel, list, task.id)), QString());
        QVERIFY(saveEvidence(panel, QStringLiteral("selected-failed-after-reset")));
    }

    void tc_2b3_task_selected_cancel_matches_current_capability()
    {
        auto &manager = AsyncTaskManager::instance();
        GatedTask task;
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Running, 3000);
        AsyncTaskPanelDialog panel;
        sizePanelForMode(panel);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        QVERIFY(list);
        manager.updateTaskPhase(task.id, QStringLiteral("读取内容"), 1, 2, 1234, 10000);
        list->setCurrentIndex(indexForTask(list, task.id));
        auto *cancelSelected = panel.findChild<QAbstractButton *>(
            QStringLiteral("cancelSelectedTaskButton"));
        QVERIFY(cancelSelected && cancelSelected->isEnabled());
        if (!nativeEvidenceMode())
            QVERIFY(paintedRow(list, indexForTask(list, task.id)).contains(QStringLiteral("取消")));
        const AsyncTask beforeCapabilityChange = manager.taskById(task.id);
        StatusBarTaskWidget taskChip;
        taskChip.show();
        QVERIFY(QTest::qWaitForWindowExposed(&taskChip));
        auto *chipButton = taskChip.findChild<QToolButton *>(QStringLiteral("StatusBarTaskWidget_btn"));
        QVERIFY(chipButton);
        manager.setTaskCancellable(task.id, false);
        QVERIFY(!indexForTask(list, task.id).data(AsyncTaskManager::TaskCancellableRole).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!cancelSelected->isEnabled(), 1000);
        const QString noncancellableRow = paintedRow(list, indexForTask(list, task.id));
        if (!nativeEvidenceMode())
            QVERIFY2(!noncancellableRow.contains(QStringLiteral("取消")), qPrintable(noncancellableRow));
        const AsyncTask noncancellable = manager.taskById(task.id);
        QCOMPARE(noncancellable.state, AsyncTaskState::Running);
        QCOMPARE(noncancellable.progressPhase, beforeCapabilityChange.progressPhase);
        QCOMPARE(noncancellable.progressPhaseIndex, beforeCapabilityChange.progressPhaseIndex);
        QCOMPARE(noncancellable.progressDone, beforeCapabilityChange.progressDone);
        QCOMPARE(noncancellable.progressTotal, beforeCapabilityChange.progressTotal);
        QCOMPARE(manager.cancellableCount(), 0);
        QCOMPARE(manager.runningCount(), 1);
        taskChip.onThreadCountChanged(manager.activeThreadCount(), manager.maximumThreadCount());
        QVERIFY2(chipButton->toolTip().contains(QStringLiteral("活跃任务 1")), qPrintable(chipButton->toolTip()));
        QVERIFY(!chipButton->toolTip().contains(QStringLiteral("线程")));
        QVERIFY(chipButton->isVisible());
        QVERIFY(saveEvidence(panel, QStringLiteral("selected-running-not-cancellable")));

        manager.setTaskCancellable(task.id, true);
        QVERIFY(indexForTask(list, task.id).data(AsyncTaskManager::TaskCancellableRole).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(cancelSelected->isEnabled(), 1000);
        if (!nativeEvidenceMode())
            QVERIFY(paintedRow(list, indexForTask(list, task.id)).contains(QStringLiteral("取消")));
        QCOMPARE(manager.taskById(task.id).state, AsyncTaskState::Running);
        QCOMPARE(manager.taskById(task.id).progressPhase, beforeCapabilityChange.progressPhase);
        QCOMPARE(manager.taskById(task.id).percentHundredths(), 1234);
        QCOMPARE(list->currentIndex().data(AsyncTaskManager::TaskIdRole).toLongLong(), task.id);
    }

    void tc_2b3_task_default_surface_hides_diagnostics_including_delegate()
    {
        auto &manager = AsyncTaskManager::instance();
        GatedTask task;
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Running, 3000);
        task.fail();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Failed, 3000);
        AsyncTaskPanelDialog panel;
        sizePanelForMode(panel);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        QVERIFY(list);
        list->setCurrentIndex(indexForTask(list, task.id));
        const QString painted = paintedRow(list, indexForTask(list, task.id));
        if (!nativeEvidenceMode())
            QVERIFY2(painted.contains(kTaskTitle), "The paint recorder must observe the real task row");
        const QString visible = defaultSurfaceText(panel, list, task.id);
        qInfo().noquote() << "2B3 actual painted row:" << painted;
        QVERIFY(saveEvidence(panel, QStringLiteral("default-failed-diagnostics-boundary")));
        // The old delegate leaks source/message before any new object exists;
        // test the actual paint sink separately from QLabel/text edit discovery.
        if (!nativeEvidenceMode()) QCOMPARE(diagnosticsFound(painted), QString());
        QCOMPARE(diagnosticsFound(visible), QString());
        QVERIFY2(visible.contains(QStringLiteral("未完成")), qPrintable(visible));
        QVERIFY2(visible.contains(QStringLiteral("重试"))
            || visible.contains(QStringLiteral("查看详情"))
            || visible.contains(QStringLiteral("技术详情")), qPrintable(visible));
        QCOMPARE(manager.taskById(task.id).error.toString(), diagnosticFailure().toString());
        auto *item = taskItemInterface(QAccessible::queryAccessibleInterface(list));
        QVERIFY(item);
        QCOMPARE(diagnosticsFound(accessibleText(item)), QString());
    }

    void tc_2b3_task_technical_details_keyboard_and_accessibility()
    {
        auto &manager = AsyncTaskManager::instance();
        GatedTask task;
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Running, 3000);
        task.fail();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Failed, 3000);
        AsyncTaskPanelDialog panel;
        sizePanelForMode(panel);
        panel.show();
        panel.raise();
        panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        QVERIFY(list);
        list->setCurrentIndex(indexForTask(list, task.id));
        QVERIFY(saveEvidence(panel, QStringLiteral("details-before-keyboard-expand")));
        auto *toggle = panel.findChild<QAbstractButton *>(QStringLiteral("backgroundTaskDetailsToggle"));
        auto *technical = panel.findChild<QWidget *>(QStringLiteral("backgroundTaskTechnicalDetails"));
        auto *details = panel.findChild<QPlainTextEdit *>(QStringLiteral("backgroundTaskDetails"));
        auto *threads = panel.findChild<QLabel *>(QStringLiteral("taskSummaryThreads"));
        QVERIFY2(toggle && technical && details && threads,
            "Keep existing detail/thread objects inside an explicit default-collapsed technical section");
        QVERIFY(!technical->isVisible());
        QVERIFY(!details->isVisible());
        QVERIFY(!threads->isVisible());
        auto *technicalInterface = QAccessible::queryAccessibleInterface(technical);
        QVERIFY(technicalInterface && technicalInterface->state().invisible);
        auto *toggleInterface = QAccessible::queryAccessibleInterface(toggle);
        QVERIFY(toggleInterface);
        QVERIFY(accessibleText(toggleInterface).contains(QStringLiteral("技术详情")));
        QCOMPARE(diagnosticsFound(defaultSurfaceText(panel, list, task.id)), QString());

        list->setFocus();
        for (int step = 0; step < 32 && QApplication::focusWidget() != toggle; ++step)
            QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &panel, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), toggle);
        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(technical->isVisible() && details->isVisible() && threads->isVisible(), 1000);
        QVERIFY(!QAccessible::queryAccessibleInterface(technical)->state().invisible);
        toggleInterface = QAccessible::queryAccessibleInterface(toggle);
        QVERIFY(toggleInterface->state().checked || toggleInterface->state().expanded
            || accessibleText(toggleInterface).contains(QStringLiteral("收起")));
        const QString full = details->toPlainText();
        QVERIFY2(full.contains(diagnosticFailure().toString()), qPrintable(full));
        QVERIFY2(full.contains(kDescription) && full.contains(QString::number(kBatchId)), qPrintable(full));
        QVERIFY2(full.contains(QString::number(task.id)), qPrintable(full));
        const int activeThreads = manager.activeThreadCount();
        const int maximumThreads = manager.maximumThreadCount();
        panel.refreshThreadOccupancy(activeThreads, maximumThreads);
        QVERIFY(threads->text().contains(QStringLiteral("线程")));
        QVERIFY2(threads->text().contains(QStringLiteral("%1/%2").arg(activeThreads).arg(maximumThreads)),
            qPrintable(threads->text()));
        const QString expandedSpoken = visibleAccessibleText(QAccessible::queryAccessibleInterface(&panel));
        QVERIFY2(expandedSpoken.contains(kRawMessage) && expandedSpoken.contains(kSource), qPrintable(expandedSpoken));
        QVERIFY(saveEvidence(panel, QStringLiteral("details-expanded-keyboard")));

        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(!technical->isVisible() && !details->isVisible(), 1000);
        QVERIFY(QAccessible::queryAccessibleInterface(technical)->state().invisible);
        toggleInterface = QAccessible::queryAccessibleInterface(toggle);
        QVERIFY(!toggleInterface->state().checked && !toggleInterface->state().expanded);
        QCOMPARE(diagnosticsFound(defaultSurfaceText(panel, list, task.id)), QString());
        for (int step = 0; step < 20; ++step) {
            QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &panel, Qt::Key_Tab);
            QVERIFY(!technical->isAncestorOf(QApplication::focusWidget()));
        }
        QCOMPARE(manager.taskById(task.id).error.toString(), diagnosticFailure().toString());
        QVERIFY(saveEvidence(panel, QStringLiteral("details-collapsed-again")));
    }

    void tc_2b3_task_known_user_message_preserves_technical_reading()
    {
        auto &manager = AsyncTaskManager::instance();
        const QString knownMessage = QStringLiteral("无法读取 <b>报告</b>。请重新打开文件后重试。");
        const QString selectedMarker = QStringLiteral("SELECTED_DESCRIPTION_2B3_UNIQUE");
        const QString description = kDescription + QLatin1Char('\n') + selectedMarker
            + QStringLiteral("\n技术记录：仅用于验证阅读位置，不作为默认任务说明。").repeated(35);
        GatedTask task(knownMessage, description);
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Running, 3000);
        AsyncTaskPanelDialog panel;
        sizePanelForMode(panel);
        panel.show();
        panel.raise();
        panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto *list = panel.findChild<QListView *>(QStringLiteral("backgroundTaskList"));
        auto *summary = panel.findChild<QLabel *>(QStringLiteral("backgroundTaskSummary"));
        auto *toggle = panel.findChild<QAbstractButton *>(QStringLiteral("backgroundTaskDetailsToggle"));
        auto *details = panel.findChild<QPlainTextEdit *>(QStringLiteral("backgroundTaskDetails"));
        QVERIFY(list && summary && toggle && details);
        manager.updateTaskPhase(task.id, QStringLiteral("读取内容"), 1, 2, 1234, 10000);
        list->setCurrentIndex(indexForTask(list, task.id));
        toggle->setFocus();
        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(details->isVisible(), 1000);
        QVERIFY(saveEvidence(panel, QStringLiteral("reading-before-selection")));
        const int markerStart = details->toPlainText().indexOf(selectedMarker);
        QVERIFY(markerStart >= 0);
        QCOMPARE(details->toPlainText().count(selectedMarker), 1);
        QTextCursor selection = details->textCursor();
        selection.setPosition(markerStart + selectedMarker.size());
        selection.setPosition(markerStart, QTextCursor::KeepAnchor); // Reverse selection.
        details->setTextCursor(selection);
        QTRY_VERIFY_WITH_TIMEOUT(details->verticalScrollBar()->maximum() > 0, 1000);
        details->verticalScrollBar()->setValue(qMax(1, details->verticalScrollBar()->maximum() / 2));
        const int vertical = details->verticalScrollBar()->value();
        const int horizontal = details->horizontalScrollBar()->value();
        QVERIFY(vertical > 0);
        QCOMPARE(details->textCursor().selectedText(), selectedMarker);
        QVERIFY(details->textCursor().anchor() > details->textCursor().position());

        // This inserts a longer prefix before the description. Retaining raw
        // absolute offsets is insufficient: the same text must stay selected.
        const QString longerPhase = QStringLiteral("整理并验证所有已读取内容，正在准备可供查看的结果");
        manager.updateTaskPhase(task.id, longerPhase, 2, 2, 1, 3);
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains(longerPhase)
            && summary->text().contains(QStringLiteral("33.33%")), 1000);
        QVERIFY(saveEvidence(panel, QStringLiteral("reading-after-longer-phase")));
        QCOMPARE(details->textCursor().selectedText(), selectedMarker);
        QVERIFY(details->textCursor().anchor() > details->textCursor().position());
        QCOMPARE(details->textCursor().selectionStart(), details->toPlainText().indexOf(selectedMarker));
        QCOMPARE(details->verticalScrollBar()->value(), qMin(vertical, details->verticalScrollBar()->maximum()));
        QCOMPARE(details->horizontalScrollBar()->value(), qMin(horizontal, details->horizontalScrollBar()->maximum()));
        QCOMPARE(list->currentIndex().data(AsyncTaskManager::TaskIdRole).toLongLong(), task.id);

        task.fail();
        QTRY_COMPARE_WITH_TIMEOUT(manager.taskById(task.id).state, AsyncTaskState::Failed, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(summary->text().contains(knownMessage), 1000);
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        QCOMPARE(manager.taskById(task.id).error.userMessage, knownMessage);
        QCOMPARE(manager.taskById(task.id).error.toString(), diagnosticFailure().toString());
        QVERIFY(details->toPlainText().contains(diagnosticFailure().toString()));
        QCOMPARE(details->textCursor().selectedText(), selectedMarker);
        QVERIFY(details->textCursor().anchor() > details->textCursor().position());
        const QModelIndex failedIndex = indexForTask(list, task.id);
        QVERIFY(failedIndex.data(Qt::AccessibleTextRole).toString().contains(knownMessage));
        auto *item = taskItemInterface(QAccessible::queryAccessibleInterface(list));
        QVERIFY(item);
        QVERIFY2(accessibleText(item).contains(knownMessage), qPrintable(accessibleText(item)));
        if (!nativeEvidenceMode()) {
            const QString painted = paintedRow(list, failedIndex);
            QVERIFY2(painted.contains(knownMessage), qPrintable(painted));
        }
        QVERIFY(saveEvidence(panel, QStringLiteral("known-failure-technical-reading")));
        toggle->setFocus();
        QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(!details->isVisible(), 1000);
        QCOMPARE(diagnosticsFound(defaultSurfaceText(panel, list, task.id)), QString());
        QVERIFY(saveEvidence(panel, QStringLiteral("known-failure-public-summary")));
    }
};

QTEST_MAIN(TaskProductLanguageSmoke)
#include "smoke_task_product_language.moc"
