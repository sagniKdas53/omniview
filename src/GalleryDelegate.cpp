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

    // Card Colors matching omniview-rs
    QColor bgColor;
    QColor borderColor;
    double borderWidth = 1.0;

    if (m_darkMode) {
        if (isSelected) {
            bgColor = QColor(18, 38, 62);      // rgb(18, 38, 62)
            borderColor = QColor(0, 210, 255); // rgb(0, 210, 255)
            borderWidth = 2.0;
        } else if (isHovered) {
            bgColor = QColor(34, 40, 56);      // rgb(34, 40, 56)
            borderColor = QColor(90, 160, 255);// rgb(90, 160, 255)
            borderWidth = 1.5;
        } else {
            bgColor = QColor(22, 25, 34);      // rgb(22, 25, 34)
            borderColor = QColor(45, 50, 65);  // rgb(45, 50, 65)
            borderWidth = 1.0;
        }
    } else {
        if (isSelected) {
            bgColor = QColor(224, 242, 254);   // rgb(224, 242, 254)
            borderColor = QColor(0, 210, 255); // rgb(0, 210, 255)
            borderWidth = 2.0;
        } else if (isHovered) {
            bgColor = QColor(235, 242, 252);   // rgb(235, 242, 252)
            borderColor = QColor(40, 120, 230);// rgb(40, 120, 230)
            borderWidth = 1.5;
        } else {
            bgColor = QColor(248, 250, 252);   // rgb(248, 250, 252)
            borderColor = QColor(220, 225, 235);// rgb(220, 225, 235)
            borderWidth = 1.0;
        }
    }

    // Draw card background with 6px corner radius
    QPainterPath bgPath;
    bgPath.addRoundedRect(cardRect, 6.0, 6.0);
    painter->fillPath(bgPath, bgColor);

    painter->setPen(QPen(borderColor, borderWidth));
    painter->drawPath(bgPath);

    // Thumbnail Area
    const double pad = 5.0;
    const QRectF imgArea(cardRect.left() + pad, cardRect.top() + pad,
                         cardRect.width() - pad * 2, m_cardSize - pad * 2);

    // Thumbnail placeholder background (4px corner radius)
    QPainterPath imgClipPath;
    imgClipPath.addRoundedRect(imgArea, 4.0, 4.0);
    painter->fillPath(imgClipPath, m_darkMode ? QColor(28, 32, 44) : QColor(230, 235, 245));

    const QString path = index.data(GalleryRoles::PathRole).toString();
    QPixmap pix = m_thumbMgr ? m_thumbMgr->getCachedPixmap(path) : QPixmap();

    if (pix.isNull()) {
        if (m_thumbMgr) {
            m_thumbMgr->requestPriorityThumbnail(path);
        }
        painter->setPen(m_darkMode ? QColor(80, 95, 120) : QColor(160, 175, 195));
        QFont iconFont = painter->font();
        iconFont.setPointSize(22);
        painter->setFont(iconFont);
        painter->drawText(imgArea, Qt::AlignCenter, QStringLiteral("🖼"));
    } else {
        painter->save();
        painter->setClipPath(imgClipPath);
        const QSize targetSize = pix.size().scaled(imgArea.size().toSize(), Qt::KeepAspectRatio);
        const double px = imgArea.left() + (imgArea.width() - targetSize.width()) / 2.0;
        const double py = imgArea.top() + (imgArea.height() - targetSize.height()) / 2.0;
        const QRectF targetRect(px, py, targetSize.width(), targetSize.height());
        painter->drawPixmap(targetRect, pix, pix.rect());
        painter->restore();
    }

    // Favorite Star indicator in upper right
    const bool isFav = index.data(GalleryRoles::IsFavoriteRole).toBool();
    if (isFav) {
        const QRectF starRect(cardRect.right() - 26, cardRect.top() + 6, 20, 20);
        QPainterPath starPill;
        starPill.addRoundedRect(starRect, 10, 10);
        painter->fillPath(starPill, QColor(0, 0, 0, 160));
        painter->setPen(QColor(250, 204, 21)); // Gold
        QFont f = painter->font();
        f.setPointSize(10);
        f.setBold(true);
        painter->setFont(f);
        painter->drawText(starRect, Qt::AlignCenter, QStringLiteral("⭐"));
    }

    // Dimensions badge in bottom right of thumbnail
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
        const QRectF badgeRect(imgArea.right() - textW - 8, imgArea.bottom() - 17, textW + 6, 15);
        QPainterPath bp;
        bp.addRoundedRect(badgeRect, 4, 4);
        painter->fillPath(bp, QColor(0, 0, 0, 170));
        painter->setPen(QColor(220, 225, 235));
        painter->drawText(badgeRect, Qt::AlignCenter, dimText);
    }

    // Batch Selection Checkbox Indicator (20×20 with 4px corner radius)
    if (m_selectMode || isSelected) {
        const QRectF cbRect(cardRect.left() + 7, cardRect.top() + 7, 20, 20);
        QPainterPath cbPath;
        cbPath.addRoundedRect(cbRect, 4.0, 4.0);
        if (isSelected) {
            painter->fillPath(cbPath, QColor(0, 180, 255)); // rgb(0, 180, 255)
            painter->setPen(QColor(255, 255, 255));
            QFont f = painter->font();
            f.setPointSize(10);
            f.setBold(true);
            painter->setFont(f);
            painter->drawText(cbRect, Qt::AlignCenter, QStringLiteral("✓"));
        } else {
            painter->fillPath(cbPath, QColor(0, 0, 0, 150));
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
    painter->setPen(m_darkMode ? QColor(220, 225, 235) : QColor(30, 40, 55));

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
