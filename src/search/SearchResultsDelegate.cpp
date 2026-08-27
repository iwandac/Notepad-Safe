#include "SearchResultsDelegate.h"

#include "SearchResultsModel.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTreeView>

namespace {

struct VisualMatchRange {
    int start = 0;
    int length = 0;
    int sourceIndex = -1;
};

struct PreparedContext {
    QString text;
    QVector<VisualMatchRange> matches;
};

int previousUtf16Boundary(const QString &text, int position)
{
    int candidate = qMax(0, position - 1);
    if (candidate > 0 && text.at(candidate).isLowSurrogate()
        && text.at(candidate - 1).isHighSurrogate()) {
        --candidate;
    }
    return candidate;
}

PreparedContext prepareContext(const QModelIndex &index, int maxWidth,
                               const QFontMetrics &metrics)
{
    PreparedContext prepared;
    const QString text = index.data(SearchResultsModel::ContextRole).toString();
    const QVariantList ranges =
        index.data(SearchResultsModel::MatchPositionsRole).toList();
    if (text.isEmpty() || ranges.isEmpty() || maxWidth <= 0) {
        prepared.text = metrics.elidedText(text, Qt::ElideRight, qMax(0, maxWidth));
        return prepared;
    }

    const QVariantMap primary = ranges.first().toMap();
    const int primaryStart = qBound(
        0, primary.value(QStringLiteral("start")).toInt(), text.size());
    const int primaryLength = qBound(
        0, primary.value(QStringLiteral("length")).toInt(),
        text.size() - primaryStart);
    const int primaryEnd = primaryStart + primaryLength;
    const QString ellipsis(QChar(0x2026));
    const int matchWidth = metrics.horizontalAdvance(
        text.mid(primaryStart, primaryLength));
    const int ellipsisWidth = metrics.horizontalAdvance(ellipsis);
    const bool hasLeading = primaryStart > 0;
    const bool hasTrailing = primaryEnd < text.size();
    int decorationBudget = qMax(0, maxWidth - matchWidth);
    bool showLeadingEllipsis = false;
    bool showTrailingEllipsis = false;
    if (hasLeading && hasTrailing && decorationBudget >= 2 * ellipsisWidth) {
        showLeadingEllipsis = true;
        showTrailingEllipsis = true;
        decorationBudget -= 2 * ellipsisWidth;
    } else if (hasLeading && decorationBudget >= ellipsisWidth) {
        showLeadingEllipsis = true;
        decorationBudget -= ellipsisWidth;
    } else if (hasTrailing && decorationBudget >= ellipsisWidth) {
        showTrailingEllipsis = true;
        decorationBudget -= ellipsisWidth;
    }

    const int prefixBudget = decorationBudget * 2 / 5;
    const int suffixBudget = decorationBudget - prefixBudget;
    int sourceStart = primaryStart;
    while (sourceStart > 0) {
        const int candidate = previousUtf16Boundary(text, sourceStart);
        if (metrics.horizontalAdvance(
                text.mid(candidate, primaryStart - candidate)) > prefixBudget) {
            break;
        }
        sourceStart = candidate;
    }

    int sourceEnd = primaryEnd;
    while (sourceEnd < text.size()) {
        int candidate = sourceEnd + 1;
        if (candidate < text.size() && text.at(sourceEnd).isHighSurrogate()
            && text.at(candidate).isLowSurrogate()) {
            ++candidate;
        }
        if (metrics.horizontalAdvance(
                text.mid(primaryEnd, candidate - primaryEnd)) > suffixBudget) {
            break;
        }
        sourceEnd = candidate;
    }

    const QString leading = sourceStart > 0 && showLeadingEllipsis
        ? ellipsis : QString();
    const QString trailing = sourceEnd < text.size() && showTrailingEllipsis
        ? ellipsis : QString();
    prepared.text = leading + text.mid(sourceStart, sourceEnd - sourceStart)
        + trailing;
    const int visibleContentEnd = prepared.text.size() - trailing.size();

    for (int i = 0; i < ranges.size(); ++i) {
        const QVariantMap range = ranges.at(i).toMap();
        const int start = qBound(0, range.value(QStringLiteral("start")).toInt(),
                                 text.size());
        const int length = qMax(0, range.value(QStringLiteral("length")).toInt());
        const int end = qBound(start, start + length, text.size());
        int visualStart = leading.size() + start - sourceStart;
        int visualEnd = leading.size() + end - sourceStart;
        if (visualEnd <= 0 || visualStart >= visibleContentEnd)
            continue;
        visualStart = qBound(0, visualStart, visibleContentEnd);
        visualEnd = qBound(visualStart, visualEnd, visibleContentEnd);
        if (visualEnd > visualStart)
            prepared.matches.append({visualStart, visualEnd - visualStart, i});
    }
    return prepared;
}

class SearchResultItemDelegate final : public QStyledItemDelegate {
public:
    explicit SearchResultItemDelegate(QObject *parent)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *tree = qobject_cast<const QTreeView *>(parent());
        const bool trackedHover = tree
            && tree->property("searchHoverRow").toInt() == index.row()
            && tree->property("searchHoverHasParent").toBool()
                   == index.parent().isValid()
            && (!index.parent().isValid()
                || tree->property("searchHoverParentRow").toInt()
                       == index.parent().row());
        const bool hoveredRow = trackedHover
            || (option.state & QStyle::State_MouseOver);
        const bool selected = option.state & QStyle::State_Selected;
        const auto paintHover = [&] {
            if (!hoveredRow || selected)
                return;
            QColor hover = option.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(option.rect, hover);
        };
        if (index.column() != SearchResultsModel::COL_CONTEXT) {
            QStyledItemDelegate::paint(painter, option, index);
            paintHover();
            return;
        }

        const QVariantList encodedRanges =
            index.data(SearchResultsModel::MatchPositionsRole).toList();
        if (index.data(SearchResultsModel::ContextRole).isNull()
            || encodedRanges.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            paintHover();
            return;
        }

        painter->save();
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        paintHover();

        constexpr int HorizontalPadding = 6;
        const QRect textRect = option.rect.adjusted(HorizontalPadding, 0,
                                                    -HorizontalPadding, 0);
        const PreparedContext prepared = prepareContext(
            index, textRect.width(), option.fontMetrics);
        QTextLayout textLayout(prepared.text, option.font);
        QVector<QTextLayout::FormatRange> formats;
        for (const VisualMatchRange &range : prepared.matches) {
            QTextCharFormat format;
            format.setFontWeight(QFont::DemiBold);
            format.setBackground(selected ? QColor(255, 255, 255, 72)
                                          : QColor(255, 214, 64, 155));
            format.setForeground(selected
                ? option.palette.color(QPalette::HighlightedText)
                : QColor(132, 72, 0));
            formats.append({range.start, range.length, format});
        }
        textLayout.setFormats(formats);
        textLayout.beginLayout();
        QTextLine line = textLayout.createLine();
        if (line.isValid())
            line.setLineWidth(textRect.width());
        textLayout.endLayout();
        painter->setPen(selected
            ? option.palette.color(QPalette::HighlightedText)
            : option.palette.color(QPalette::Text));
        if (line.isValid()) {
            const qreal y = textRect.top()
                + (textRect.height() - line.height()) / 2.0;
            textLayout.draw(painter, QPointF(textRect.left(), y));
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        hint.setHeight(option.fontMetrics.height() + 8);
        return hint;
    }
};

} // namespace

QAbstractItemDelegate *createSearchResultsDelegate(QObject *parent)
{
    return new SearchResultItemDelegate(parent);
}

int hitTestSearchResultMatch(const QModelIndex &index, int xInColumn,
                             int columnWidth, const QFontMetrics &metrics)
{
    constexpr int HorizontalPadding = 6;
    const int x = xInColumn - HorizontalPadding;
    const PreparedContext prepared = prepareContext(
        index, qMax(0, columnWidth - 2 * HorizontalPadding), metrics);
    if (x < 0)
        return -1;
    for (const VisualMatchRange &range : prepared.matches) {
        const int startX = metrics.horizontalAdvance(
            prepared.text.left(range.start));
        const int endX = metrics.horizontalAdvance(
            prepared.text.left(range.start + range.length));
        if (x >= startX && x <= endX)
            return range.sourceIndex;
    }
    return -1;
}
