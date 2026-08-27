#include "utils/IconOnlyDelegate.h"
#include <QPainterPath>
#include "widget.h"

void IconOnlyDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen); //取消边框
    // option.rect.size() == QListWidgetItem::sizeHint(); its square top is the icon slot, the band
    // below it is reserved for the app name label drawn by Widget
    const QRect cell(option.rect.topLeft(), QSize(option.rect.width(), option.rect.width()));
    if (option.state & QStyle::State_Selected) {
        QPainterPath path;
        path.addRoundedRect(cell, radius, radius);
        painter->setBrush(selectedColor);
        painter->drawPath(path);

        // inner shadow: 1px-stepped outlines shrinking inward, alpha falling off quadratically,
        // clipped to the path so the corners stay rounded
        constexpr int blur = 10;
        constexpr int strength = 15;
        painter->save();
        painter->setClipPath(path);
        painter->setBrush(Qt::NoBrush);
        for (int i = 0; i < blur; ++i) {
            const qreal falloff = 1.0 - qreal(i) / blur;
            painter->setPen(QPen(QColor(0, 0, 0, int(strength * falloff * falloff)), 2));
            painter->drawRoundedRect(QRectF(cell).adjusted(i, i, -i, -i), radius, radius);
        }
        painter->restore();
        painter->setPen(Qt::NoPen);
    } else if (option.state & QStyle::State_MouseOver) {
        painter->setBrush(hoverColor);
        painter->drawRoundedRect(cell, radius, radius);
    }

    // 居中绘制图标
    auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    if (!icon.isNull()) {
        QRect iconRect{{}, option.decorationSize}; // QListWidget::iconSize()
        iconRect.moveCenter(cell.center());
        icon.paint(painter, iconRect);
    }

    // draw badge
    auto num = qvariant_cast<WindowGroup>(index.data(Qt::UserRole)).windows.size();
    if (num > 1) {
        auto text = QString::number(num);
        const auto extraWidth = 8 * (text.size() - 1);
        constexpr auto R = 12;
        auto badgeCenter = cell.topRight() + QPoint(-(R + 3), R + 3);
        // extra Width for extra number
        auto badgeRect = QRect(badgeCenter + QPoint(-R - extraWidth, -R), QSize(2 * R + extraWidth, 2 * R));
        painter->setPen(QColor(200, 200, 200, 50));
        painter->setBrush(QColor(133, 114, 97)); // learn from iOS
        painter->drawRoundedRect(badgeRect, R, R);

        QFont font{"Microsoft YaHei"};
        font.setPointSizeF(12.8);
        font.setBold(true);
        painter->setFont(font);
        painter->setPen(QColor(214, 192, 171));
        painter->drawText(badgeRect, Qt::AlignCenter, text);
    }
}
