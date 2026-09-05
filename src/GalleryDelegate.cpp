#include "GalleryDelegate.h"
#include "GalleryModel.h"

#include <QPainterPath>
#include <QFontMetrics>
#include <QDateTime>

namespace OmniView {

GalleryDelegate::GalleryDelegate(ThumbnailManager* thumbMgr, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_thumbMgr(thumbMgr)
{
}

void GalleryDelegate::setCardSize(int size) {
    m_cardSize = size;
}

void GalleryDelegate::setSelectMode(bool enabled) {
    m_selectMode = enabled;
}

void GalleryDelegate::setDarkMode(bool dark) {
    m_darkMode = dark;
}

QSize GalleryDelegate::sizeHint(const QStyleOptionViewItem& /*option*/, const QModelIndex& /*index*/) const {
    return QSize(m_cardSize, m_cardSize + 56);
}

void GalleryDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (!index.isValid()) return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const bool isSelected = (option.state & QStyle::State_Selected);
    const bool isHovered = (option.state & QStyle::State_MouseOver);

    const QRectF cardRect = QRectF(option.rect).adjusted(3, 3, -3, -3);

    // Card Colors
    QColor bgColor = m_darkMode ? QColor(30, 34, 48) : QColor(255, 255, 255);
    QColor borderColor = m_darkMode ? QColor(42, 50, 75) : QColor(226, 232, 240);

    if (isSelected) {
        borderColor = QColor(99, 102, 241); // indigo
        bgColor = m_darkMode ? QColor(37, 42, 60) : QColor(238, 242, 255);
    } else if (isHovered) {
        borderColor = m_darkMode ? QColor(67, 78, 110) : QColor(203, 213, 225);
        bgColor = m_darkMode ? QColor(34, 39, 55) : QColor(248, 250, 252);
    }

    // Draw card background
    QPainterPath bgPath;
    bgPath.addRoundedRect(cardRect, 8.0, 8.0);
    painter->fillPath(bgPath, bgColor);

    painter->setPen(QPen(borderColor, isSelected ? 2.0 : 1.0));
    painter->drawPath(bgPath);

    // Thumbnail Area
    const double pad = 6.0;
    const QRectF imgArea(cardRect.left() + pad, cardRect.top() + pad,
                         cardRect.width() - pad * 2, m_cardSize - pad * 2);

    // Subtle thumbnail placeholder background
    QPainterPath imgClipPath;
    imgClipPath.addRoundedRect(imgArea, 6.0, 6.0);
    painter->fillPath(imgClipPath, m_darkMode ? QColor(20, 23, 33) : QColor(241, 245, 249));

    const QString path = index.data(GalleryRoles::PathRole).toString();
    QPixmap pix = m_thumbMgr ? m_thumbMgr->getCachedPixmap(path) : QPixmap();

    if (pix.isNull()) {
        if (m_thumbMgr) {
            m_thumbMgr->requestPriorityThumbnail(path);
        }
        // Draw loading placeholder
        painter->setPen(m_darkMode ? QColor(100, 116, 139) : QColor(148, 163, 184));
        QFont iconFont = painter->font();
        iconFont.setPointSize(24);
        painter->setFont(iconFont);
        painter->drawText(imgArea, Qt::AlignCenter, QStringLiteral("🖼"));
    } else {
        painter->save();
        painter->setClipPath(imgClipPath);
        const QPixmap scaled = pix.scaled(imgArea.size().toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const double px = imgArea.left() + (imgArea.width() - scaled.width()) / 2.0;
        const double py = imgArea.top() + (imgArea.height() - scaled.height()) / 2.0;
        painter->drawPixmap(QPointF(px, py), scaled);
        painter->restore();
    }

    // Favorite Star
    const bool isFav = index.data(GalleryRoles::IsFavoriteRole).toBool();
    if (isFav) {
        const QRectF starRect(cardRect.right() - 28, cardRect.top() + 8, 22, 22);
        QPainterPath starPill;
        starPill.addRoundedRect(starRect, 11, 11);
        painter->fillPath(starPill, QColor(0, 0, 0, 180));
        painter->setPen(QColor(250, 204, 21)); // Gold
        QFont f = painter->font();
        f.setPointSize(11);
        f.setBold(true);
        painter->setFont(f);
        painter->drawText(starRect, Qt::AlignCenter, QStringLiteral("⭐"));
    }

    // Dimensions badge
    const int w = index.data(GalleryRoles::WidthRole).toInt();
    const int h = index.data(GalleryRoles::HeightRole).toInt();
    if (w > 0 && h > 0) {
        const QString dimText = QStringLiteral("%1×%2").arg(w).arg(h);
        QFont f = painter->font();
        f.setPointSize(8);
        f.setBold(false);
        painter->setFont(f);
        QFontMetrics fm(f);
        const int textW = fm.horizontalAdvance(dimText);
        const QRectF badgeRect(imgArea.right() - textW - 10, imgArea.bottom() - 18, textW + 8, 16);
        QPainterPath bp;
        bp.addRoundedRect(badgeRect, 4, 4);
        painter->fillPath(bp, QColor(0, 0, 0, 170));
        painter->setPen(QColor(241, 245, 249));
        painter->drawText(badgeRect, Qt::AlignCenter, dimText);
    }

    // Checkbox in Select Mode
    if (m_selectMode) {
        const QRectF cbRect(cardRect.left() + 8, cardRect.top() + 8, 22, 22);
        QPainterPath cbPath;
        cbPath.addRoundedRect(cbRect, 11, 11);
        if (isSelected) {
            painter->fillPath(cbPath, QColor(79, 70, 229));
            painter->setPen(QColor(255, 255, 255));
            QFont f = painter->font();
            f.setPointSize(11);
            f.setBold(true);
            painter->setFont(f);
            painter->drawText(cbRect, Qt::AlignCenter, QStringLiteral("✓"));
        } else {
            painter->fillPath(cbPath, QColor(0, 0, 0, 160));
            painter->setPen(QPen(QColor(255, 255, 255, 200), 1.5));
            painter->drawPath(cbPath);
        }
    }

    // Bottom Metadata Area
    const double metaTop = imgArea.bottom() + 4;
    const QRectF metaRect(cardRect.left() + pad, metaTop, cardRect.width() - pad * 2, cardRect.bottom() - metaTop - pad);

    const QString filename = index.data(GalleryRoles::FilenameRole).toString();
    const QString subfolder = index.data(GalleryRoles::SubfolderRole).toString();
    const qint64 sizeBytes = index.data(GalleryRoles::FileSizeRole).toLongLong();

    QFont fTitle = painter->font();
    fTitle.setPointSize(9);
    fTitle.setBold(true);
    painter->setFont(fTitle);
    painter->setPen(m_darkMode ? QColor(241, 245, 249) : QColor(15, 23, 42));

    QFontMetrics fmTitle(fTitle);
    const QString elidedTitle = fmTitle.elidedText(filename, Qt::ElideMiddle, static_cast<int>(metaRect.width()));
    painter->drawText(QRectF(metaRect.left(), metaRect.top(), metaRect.width(), 16),
                      Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

    // Subtitle (folder or size)
    QFont fSub = painter->font();
    fSub.setPointSize(8);
    fSub.setBold(false);
    painter->setFont(fSub);
    painter->setPen(m_darkMode ? QColor(148, 163, 184) : QColor(100, 116, 139));

    QString subText;
    if (!subfolder.isEmpty()) {
        subText = QStringLiteral("📁 %1").arg(subfolder);
    } else {
        const double mb = static_cast<double>(sizeBytes) / (1024.0 * 1024.0);
        subText = QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }

    QFontMetrics fmSub(fSub);
    const QString elidedSub = fmSub.elidedText(subText, Qt::ElideRight, static_cast<int>(metaRect.width()));
    painter->drawText(QRectF(metaRect.left(), metaRect.top() + 18, metaRect.width(), 16),
                      Qt::AlignLeft | Qt::AlignVCenter, elidedSub);

    painter->restore();
}

} // namespace OmniView
