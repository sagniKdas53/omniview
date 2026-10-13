#include "FileActions.h"
#include "ZipUtils.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QUrl>

namespace OmniView {
namespace FileActions {

QStringList resolveLocalFiles(const QStringList& paths) {
    QStringList out;
    out.reserve(paths.size());
    for (const QString& p : paths) {
        const QString local = ZipUtils::ensureExtracted(p);
        if (!local.isEmpty() && QFileInfo(local).isFile()) {
            out.append(local);
        }
    }
    return out;
}

QMimeData* createFileMimeData(const QStringList& localFiles) {
    auto* mime = new QMimeData();
    QList<QUrl> urls;
    QStringList urlStrings;
    for (const QString& p : localFiles) {
        const QUrl url = QUrl::fromLocalFile(p);
        urls.append(url);
        urlStrings.append(QString::fromUtf8(url.toEncoded()));
    }
    mime->setUrls(urls);
    mime->setText(localFiles.join(QLatin1Char('\n')));
    mime->setData(QStringLiteral("x-special/gnome-copied-files"),
                  "copy\n" + urlStrings.join(QLatin1Char('\n')).toUtf8());
    return mime;
}

bool copyFilesToClipboard(const QStringList& paths) {
    const QStringList local = resolveLocalFiles(paths);
    if (local.isEmpty()) return false;
    QApplication::clipboard()->setMimeData(createFileMimeData(local));
    return true;
}

QPixmap makeDragBadge(const QPixmap& thumbnail, int fileCount) {
    if (thumbnail.isNull()) return QPixmap();
    constexpr int badgeMax = 120;
    const QPixmap scaled = thumbnail.scaled(badgeMax, badgeMax, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap badge(scaled.width() + 12, scaled.height() + 12);
    badge.fill(Qt::transparent);

    QPainter p(&badge);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath shape;
    shape.addRoundedRect(QRectF(1, 1, scaled.width() + 10, scaled.height() + 10), 8, 8);
    p.fillPath(shape, QColor(20, 24, 38, 230));
    p.setPen(QPen(QColor(99, 102, 241), 2));
    p.drawPath(shape);
    p.drawPixmap(6, 6, scaled);

    if (fileCount > 1) {
        const QString countStr = QStringLiteral("📦 %1 files").arg(fileCount);
        QFont f = p.font();
        f.setBold(true);
        f.setPointSize(9);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int pillW = fm.horizontalAdvance(countStr) + 12;
        const QRectF pill(badge.width() - pillW - 4, badge.height() - 22, pillW, 18);
        QPainterPath pp;
        pp.addRoundedRect(pill, 6, 6);
        p.fillPath(pp, QColor(79, 70, 229, 240));
        p.setPen(QColor(255, 255, 255));
        p.drawText(pill, Qt::AlignCenter, countStr);
    }
    p.end();
    return badge;
}

bool revealInFileManager(const QString& path) {
    QString target = path;
    QString zipPath;
    if (ZipUtils::isZipPath(path, &zipPath)) {
        target = zipPath; // highlight the archive itself
    }
    const QFileInfo fi(target);
#if defined(Q_OS_WIN)
    return QProcess::startDetached(QStringLiteral("explorer.exe"),
        {QStringLiteral("/select,") + QDir::toNativeSeparators(fi.absoluteFilePath())});
#elif defined(Q_OS_MACOS)
    return QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), fi.absoluteFilePath()});
#else
    if (QProcess::startDetached(QStringLiteral("dolphin"), {QStringLiteral("--select"), fi.absoluteFilePath()})) {
        return true;
    }
    if (QProcess::startDetached(QStringLiteral("nautilus"), {QStringLiteral("--select"), fi.absoluteFilePath()})) {
        return true;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
}

bool openWithDefaultApp(const QString& path) {
    const QString local = ZipUtils::ensureExtracted(path);
    if (local.isEmpty() || !QFileInfo(local).isFile()) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(local));
}

QString escapeMnemonics(QString text) {
    return text.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

} // namespace FileActions
} // namespace OmniView
