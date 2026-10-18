#include "ShellGlyphs.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

void paintShellGlyph(QPainter *painter, ShellGlyph glyph, const QRectF &box,
                     const QColor &color)
{
    if (!painter)
        return;
    // Glyphs are designed on a 10x10 grid centred in the box.
    const qreal side = qMin(box.width(), box.height());
    const qreal unit = side / 16.0;
    const QPointF origin = box.center() - QPointF(5 * unit, 5 * unit);
    const auto at = [&](qreal x, qreal y) { return origin + QPointF(x * unit, y * unit); };

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, qMax<qreal>(1.0, unit), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    switch (glyph) {
    case ShellGlyph::Menu:
        for (const qreal y : {1.5, 5.0, 8.5})
            painter->drawLine(at(0, y), at(10, y));
        break;
    case ShellGlyph::Minimize:
        painter->drawLine(at(0, 5), at(10, 5));
        break;
    case ShellGlyph::Maximize:
        painter->drawRect(QRectF(at(0, 0), at(10, 10)));
        break;
    case ShellGlyph::Restore:
        painter->drawRect(QRectF(at(0, 2.5), at(7.5, 10)));
        painter->drawPolyline(QPolygonF{at(2.5, 2.5), at(2.5, 0), at(10, 0), at(10, 7.5),
                                        at(7.5, 7.5)});
        break;
    case ShellGlyph::Close:
        painter->drawLine(at(0, 0), at(10, 10));
        painter->drawLine(at(10, 0), at(0, 10));
        break;
    case ShellGlyph::Lock: {
        painter->drawRoundedRect(QRectF(at(1, 4.5), at(9, 10)), unit, unit);
        QPainterPath shackle;
        shackle.moveTo(at(2.75, 4.5));
        shackle.lineTo(at(2.75, 2.75));
        shackle.arcTo(QRectF(at(2.75, 0.5), at(7.25, 5)), 180, -180);
        shackle.lineTo(at(7.25, 4.5));
        painter->drawPath(shackle);
        break;
    }
    case ShellGlyph::Eye: {
        QPainterPath eye;
        eye.moveTo(at(0, 5));
        eye.quadTo(at(5, -1), at(10, 5));
        eye.quadTo(at(5, 11), at(0, 5));
        painter->drawPath(eye);
        painter->drawEllipse(at(5, 5), 1.6 * unit, 1.6 * unit);
        break;
    }
    case ShellGlyph::Warning:
        painter->drawPolygon(QPolygonF{at(5, 0), at(10, 9.5), at(0, 9.5)});
        painter->drawLine(at(5, 3.5), at(5, 6));
        painter->drawPoint(at(5, 8));
        break;
    }
    painter->restore();
}

QIcon shellGlyphIcon(ShellGlyph glyph, const QColor &normal, const QColor &active, int size)
{
    QIcon icon;
    for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        const int pixels = qRound(size * scale);
        for (const auto &[mode, color] : {std::pair{QIcon::Normal, normal},
                                          std::pair{QIcon::Active, active}}) {
            QPixmap pixmap(pixels, pixels);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            paintShellGlyph(&painter, glyph, QRectF(0, 0, pixels, pixels), color);
            painter.end();
            pixmap.setDevicePixelRatio(scale);
            icon.addPixmap(pixmap, mode);
        }
    }
    return icon;
}
