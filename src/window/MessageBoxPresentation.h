#pragma once

#include <QAbstractButton>
#include <QMessageBox>
#include <QTextEdit>

// Only for this product's Ok-only message boxes, before exec(). Keep Qt's
// native detail control and keyboard behavior, without installing a global
// translator or relabeling caller-defined ActionRole commands.
inline void localizeProductMessageBox(QMessageBox &box)
{
    if (box.standardButtons() != QMessageBox::Ok) return;
    if (auto *ok = box.button(QMessageBox::Ok)) ok->setText(QStringLiteral("确定"));
    const auto buttons = box.buttons();
    if (box.detailedText().isEmpty() || buttons.size() != 2) return;
    for (QAbstractButton *button : buttons) {
        if (box.buttonRole(button) != QMessageBox::ActionRole) continue;
        const auto updateCaption = [&box, button]() {
            const auto *detail = box.findChild<QTextEdit *>();
            const bool expanded = detail && detail->isVisibleTo(&box);
            button->setText(expanded ? QStringLiteral("收起详情") : QStringLiteral("显示详情"));
            button->setAccessibleDescription(expanded
                ? QStringLiteral("收起原始详情，不执行其他操作。")
                : QStringLiteral("展开查看完整详情，不执行其他操作。"));
        };
        updateCaption();
        QObject::connect(button, &QAbstractButton::clicked, &box, updateCaption);
    }
}
