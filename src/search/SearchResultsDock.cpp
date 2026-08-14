#include "SearchResultsDock.h"

#include <QTreeWidget>
#include <QTreeView>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <QPalette>
#include <QMouseEvent>

// ============================================================================
//  自定义 delegate：对上下文列中的匹配关键词染色
// ============================================================================
class SearchResultItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit SearchResultItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // 只对 COL_CONTEXT 列做特殊绘制
        if (index.column() != SearchResultsModel::COL_CONTEXT) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        auto* model = qobject_cast<const SearchResultsModel*>(index.model());
        if (!model) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        painter->save();

        // 背景（交替行颜色 + 选中高亮）
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, opt.palette.highlight());
        } else if (opt.features & QStyleOptionViewItem::Alternate) {
            painter->fillRect(opt.rect, opt.palette.alternateBase());
        } else {
            painter->fillRect(opt.rect, opt.palette.base());
        }

        // 获取分组数据
        const GroupedSearchLine& g = model->groupedLineAt(index.row());
        QString text = QString::fromUtf8(g.lineContext);

        // 文字区域（留 padding）
        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);
        QFontMetrics fm(opt.font);
        int ascent = fm.ascent();
        int y = opt.rect.y() + (opt.rect.height() - fm.height()) / 2 + ascent;

        // 先画匹配背景
        QColor hlBg(0xFF, 0xFF, 0x00, 0xC0);  // 半透明黄
        QColor hlFg(0xFF, 0x00, 0x00);         // 红
        QColor normalFg = (opt.state & QStyle::State_Selected)
                          ? opt.palette.color(QPalette::HighlightedText)
                          : opt.palette.color(QPalette::Text);

        // 计算文本宽度限制（超出显示区域的用省略号）
        int maxWidth = textRect.width();
        QString elidedText = fm.elidedText(text, Qt::ElideRight, maxWidth);

        // 逐字符绘制，匹配区域用高亮色
        int x = textRect.left();
        int charWidth = fm.horizontalAdvance(QLatin1Char('0'));  // 等宽假设

        // 构建「是否高亮」的字符位图
        // matchPositions 存的是字节偏移 (col)，需要转成 UTF-16 字符索引
        // P1 简化：按字节偏移近似为字符偏移（ASCII 场景准确，非 ASCII 有偏差但可接受）
        QVector<bool> highlightMask(text.size(), false);
        for (const auto& pos : g.matchPositions) {
            int col = pos.first;
            int len = pos.second;
            if (col < 0) col = 0;
            if (col >= text.size()) continue;
            int end = qMin(col + len, text.size());
            for (int i = col; i < end; ++i)
                highlightMask[i] = true;
        }

        // 逐字符绘制
        painter->setFont(opt.font);
        int drawnWidth = 0;
        for (int i = 0; i < elidedText.size() && i < text.size(); ++i) {
            QChar ch = elidedText[i];
            int w = fm.horizontalAdvance(ch);
            if (x + w > textRect.right()) break;

            if (highlightMask[i]) {
                // 画高亮背景
                painter->fillRect(QRect(x, opt.rect.y(), w, opt.rect.height()), hlBg);
                painter->setPen(hlFg);
            } else {
                painter->setPen(normalFg);
            }
            painter->drawText(x, y, ch);
            x += w;
            drawnWidth += w;
        }

        // 如果有省略号，绘制它
        if (elidedText.size() < text.size() && drawnWidth < maxWidth) {
            painter->setPen(normalFg);
            QString ellipsis = QStringLiteral("…");
            painter->drawText(x, y, ellipsis);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

// ============================================================================
//  SearchResultsDock 实现
// ============================================================================
SearchResultsDock::SearchResultsDock(QWidget *parent)
    : QDockWidget(QStringLiteral("搜索结果"), parent)
{
    // ⚠️ 关键修复：禁用浮动和移动，防止双击 / 拖动后嵌入错位
    //  - Fixed: 允许用户关闭/显示，不允许脱离位置
    //  - setAllowedAreas: 仅允许底部和顶部（底部初始）
    //  - MainWindow::addDockWidget(Qt::BottomDockWidgetArea) 已在 MainWindow 里设置，这里再次约束
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable);

    auto *container = new QWidget(this);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    m_summary = new QLabel(container);
    layout->addWidget(m_summary);

    // stack 切换：L1 QTreeWidget / L2 QTreeView
    m_stack = new QStackedWidget(container);

    // --- L1: QTreeWidget ---
    m_tree = new QTreeWidget(m_stack);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("行"), QStringLiteral("列"),
                             QStringLiteral("内容")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_stack->addWidget(m_tree);  // index 0

    // --- L2: QTreeView + SearchResultsModel ---
    m_treeView = new QTreeView(m_stack);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeView->header()->setStretchLastSection(true);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    // 设置自定义 delegate 对上下文列染色
    m_treeView->setItemDelegateForColumn(SearchResultsModel::COL_CONTEXT,
                                         new SearchResultItemDelegate(m_treeView));
    m_stack->addWidget(m_treeView);  // index 1

    layout->addWidget(m_stack);
    setWidget(container);

    // 默认显示 L1（L1 路径使用最多）
    m_stack->setCurrentIndex(0);

    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &SearchResultsDock::onItemDoubleClicked);
    connect(m_treeView, &QTreeView::doubleClicked,
            this, &SearchResultsDock::onModelDoubleClicked);

    // 关键：安装事件过滤器到 m_treeView viewport，以捕获 context 列内的点击位置
    //   —— 用于按"点击的匹配词"精准跳转（而非仅跳到第一个）
    m_treeView->viewport()->installEventFilter(this);
}

void SearchResultsDock::updateSummary(const QString &pattern, const QString &fileName,
                                      qint64 totalMatches)
{
    m_summary->setText(QStringLiteral("在 %1 中搜索 \"%2\"：找到 %3 处匹配")
                           .arg(fileName)
                           .arg(pattern)
                           .arg(totalMatches));
}

void SearchResultsDock::setResults(const QVector<SearchMatch> &matches,
                                   const QString &pattern,
                                   const QString &fileName)
{
    // 切到 L1 视图
    m_stack->setCurrentIndex(0);
    m_tree->clear();
    for (const auto &m : matches) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(m.line + 1));
        item->setText(1, QString::number(m.index + 1));
        item->setText(2, m.context);
        item->setData(0, Qt::UserRole, m.line);
        item->setData(1, Qt::UserRole, m.index);
    }
    updateSummary(pattern, fileName, qint64(matches.size()));
}

void SearchResultsDock::setResultsModel(SearchResultsModel *model,
                                        const QString &pattern,
                                        const QString &fileName)
{
    m_model = model;
    // 切到 L2 视图
    m_stack->setCurrentIndex(1);
    m_treeView->setModel(model);  // nullptr 时会清空
    if (model) {
        qint64 total = model->totalMatches();
        qint64 lines = model->groupedCount();
        m_summary->setText(QStringLiteral("在 %1 中搜索 \"%2\"：找到 %3 处匹配（%4 行）")
                               .arg(fileName)
                               .arg(pattern)
                               .arg(total)
                               .arg(lines));
    } else {
        updateSummary(pattern, fileName, 0);
    }
}

void SearchResultsDock::onItemDoubleClicked(QTreeWidgetItem *item, int /*column*/)
{
    if (!item)
        return;
    bool ok1, ok2;
    int line  = item->data(0, Qt::UserRole).toInt(&ok1);
    int index = item->data(1, Qt::UserRole).toInt(&ok2);
    if (ok1 && ok2)
        emit jumpTo(line, index);
}

bool SearchResultsDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_treeView->viewport() && m_model) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                // 计算点击落在了哪一行 + 哪一列
                QModelIndex idx = m_treeView->indexAt(me->pos());
                if (idx.isValid() && idx.column() == SearchResultsModel::COL_CONTEXT) {
                    QRect colRect = m_treeView->visualRect(idx);
                    int xInCol = me->pos().x() - colRect.left();
                    QFontMetrics fm(m_treeView->font());
                    int matchIdx = m_model->hitTestMatchIndex(idx.row(), xInCol, colRect.width(), fm);
                    m_lastHitMatchIdx = matchIdx;
                    m_lastHitRow = idx.row();
                } else {
                    m_lastHitMatchIdx = -1;
                    m_lastHitRow = -1;
                }
            }
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

void SearchResultsDock::onModelDoubleClicked(const QModelIndex &index)
{
    if (!m_model || !index.isValid()) return;
    qint64 byteOff = -1;
    int    length  = 0;
    // 如果当前按下位置是同一个行并且有命中的匹配索引，则按该索引跳转
    if (m_lastHitRow == index.row() && m_lastHitMatchIdx >= 0) {
        if (m_model->matchByteOffsetAtIndex(index.row(), m_lastHitMatchIdx, &byteOff, &length)) {
            // 命中：跳转到用户点击的具体匹配词
            emit jumpToByteOffset(byteOff, length);
            return;
        }
    }
    // 回退：跳转到第一个匹配（保持兼容）
    byteOff = m_model->matchByteOffset(index);
    length  = m_model->matchLength(index);
    if (byteOff >= 0)
        emit jumpToByteOffset(byteOff, length);
}

#include "SearchResultsDock.moc"
