#include "ViewerWindow.h"
#include "ZipUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QClipboard>
#include <QApplication>
#include <QProcess>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <QDesktopServices>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace OmniView {

DragButton::DragButton(const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Click and drag directly to Discord, Slack, or browser to attach!"));
}

void DragButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
    }
    QPushButton::mousePressEvent(event);
}

void DragButton::mouseMoveEvent(QMouseEvent* event) {
    if ((event->buttons() & Qt::LeftButton) && !m_dragStartPos.isNull()) {
        const int dist = (event->pos() - m_dragStartPos).manhattanLength();
        if (dist >= QApplication::startDragDistance()) {
            QStringList paths = m_filePaths;
            if (paths.isEmpty() && !m_filePath.isEmpty()) {
                paths.append(m_filePath);
            }
            QStringList validPaths;
            for (const QString& p : paths) {
                if (ZipUtils::isZipPath(p)) {
                    const QString extracted = ZipUtils::ensureExtracted(p);
                    if (!extracted.isEmpty() && QFile::exists(extracted)) {
                        validPaths.append(extracted);
                    }
                } else if (QFile::exists(p)) {
                    validPaths.append(p);
                }
            }
            if (!validPaths.isEmpty()) {
                auto* drag = new QDrag(this);
                auto* mimeData = new QMimeData();
                QList<QUrl> urls;
                for (const QString& p : validPaths) {
                    urls.append(QUrl::fromLocalFile(p));
                }
                mimeData->setUrls(urls);
                mimeData->setText(validPaths.join(QStringLiteral("\n")));
                QStringList urlStrings;
                for (const QUrl& u : urls) {
                    urlStrings.append(u.toString());
                }
                const QByteArray gnomeData = "copy\n" + urlStrings.join(QStringLiteral("\n")).toUtf8();
                mimeData->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeData);

                // Pre-populate clipboard for instant paste
                QApplication::clipboard()->setMimeData(mimeData);

                // Thumbnail badge preview
                QPixmap preview(validPaths.first());
                const int badgeMax = 120;
                if (!preview.isNull()) {
                    const QPixmap scaled = preview.scaled(badgeMax, badgeMax, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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

                    if (validPaths.size() > 1) {
                        const QString countStr = QStringLiteral("📦 %1 files").arg(validPaths.size());
                        QFont f = p.font();
                        f.setBold(true);
                        f.setPointSize(9);
                        p.setFont(f);
                        QFontMetrics fm(f);
                        const int pillW = fm.horizontalAdvance(countStr) + 12;
                        const QRectF pill(badge.width() - pillW - 4, badge.height() - 22, pillW, 18);
                        QPainterPath pp;
                        pp.addRoundedRect(pill, 6, 6);
                        p.fillPath(pp, QColor(79, 70, 229, 240));
                        p.setPen(QColor(255, 255, 255));
                        p.drawText(pill, Qt::AlignCenter, countStr);
                    }
                    p.end();

                    drag->setPixmap(badge);
                    drag->setHotSpot(QPoint(badge.width() / 2, badge.height() / 2));
                }
                drag->setMimeData(mimeData);
                drag->exec(Qt::CopyAction);
            }
            m_dragStartPos = QPoint();
            return;
        }
    }
    QPushButton::mouseMoveEvent(event);
}

ViewerWindow::ViewerWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("OmniView — Image Viewer"));
    resize(1100, 780);

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Toolbar
    auto* toolBar = new QWidget(this);
    auto* toolLayout = new QHBoxLayout(toolBar);
    toolLayout->setContentsMargins(12, 6, 12, 6);
    toolLayout->setSpacing(8);

    auto* btnPrev = new QPushButton(QStringLiteral("◀ Prev"), this);
    connect(btnPrev, &QPushButton::clicked, this, &ViewerWindow::showPrevious);
    toolLayout->addWidget(btnPrev);

    auto* btnNext = new QPushButton(QStringLiteral("Next ▶"), this);
    connect(btnNext, &QPushButton::clicked, this, &ViewerWindow::showNext);
    toolLayout->addWidget(btnNext);

    toolLayout->addSpacing(12);

    auto* btnFit = new QPushButton(QStringLiteral("⛶ Fit"), this);
    connect(btnFit, &QPushButton::clicked, this, &ViewerWindow::zoomFit);
    toolLayout->addWidget(btnFit);

    auto* btn100 = new QPushButton(QStringLiteral("1:1"), this);
    connect(btn100, &QPushButton::clicked, this, &ViewerWindow::zoomOriginal);
    toolLayout->addWidget(btn100);

    auto* btnZoomOut = new QPushButton(QStringLiteral("−"), this);
    btnZoomOut->setFixedWidth(32);
    connect(btnZoomOut, &QPushButton::clicked, this, &ViewerWindow::zoomOut);
    toolLayout->addWidget(btnZoomOut);

    auto* btnZoomIn = new QPushButton(QStringLiteral("+"), this);
    btnZoomIn->setFixedWidth(32);
    connect(btnZoomIn, &QPushButton::clicked, this, &ViewerWindow::zoomIn);
    toolLayout->addWidget(btnZoomIn);

    toolLayout->addSpacing(12);

    // Direct Drag-and-Drop Button
    m_dragBtn = new DragButton(QStringLiteral("🖐 Drag to Attach"), this);
    m_dragBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "    background-color: #4f46e5;"
        "    color: #ffffff;"
        "    font-weight: bold;"
        "    padding: 6px 14px;"
        "    border-radius: 4px;"
        "    border: none;"
        "}"
        "QPushButton:hover {"
        "    background-color: #4338ca;"
        "}"
    ));
    toolLayout->addWidget(m_dragBtn);

    toolLayout->addSpacing(12);

    m_btnFav = new QPushButton(QStringLiteral("⭐ Favorite"), this);
    connect(m_btnFav, &QPushButton::clicked, this, &ViewerWindow::toggleFavorite);
    toolLayout->addWidget(m_btnFav);

    auto* btnCopyFile = new QPushButton(QStringLiteral("📋 File"), this);
    btnCopyFile->setToolTip(QStringLiteral("Copy file for pasting into chat or file manager"));
    connect(btnCopyFile, &QPushButton::clicked, this, &ViewerWindow::copyFile);
    toolLayout->addWidget(btnCopyFile);

    auto* btnCopyPath = new QPushButton(QStringLiteral("🔗 Path"), this);
    btnCopyPath->setToolTip(QStringLiteral("Copy file path to clipboard"));
    connect(btnCopyPath, &QPushButton::clicked, this, &ViewerWindow::copyPath);
    toolLayout->addWidget(btnCopyPath);

    auto* btnCopyImg = new QPushButton(QStringLiteral("🖼️ Image"), this);
    btnCopyImg->setToolTip(QStringLiteral("Copy raw image bitmap to clipboard"));
    connect(btnCopyImg, &QPushButton::clicked, this, &ViewerWindow::copyImage);
    toolLayout->addWidget(btnCopyImg);

    auto* btnOpenDef = new QPushButton(QStringLiteral("↗️ Open"), this);
    btnOpenDef->setToolTip(QStringLiteral("Open in default system viewer"));
    connect(btnOpenDef, &QPushButton::clicked, this, &ViewerWindow::openDefaultViewer);
    toolLayout->addWidget(btnOpenDef);

    auto* btnShowFolder = new QPushButton(QStringLiteral("📁 Folder"), this);
    btnShowFolder->setToolTip(QStringLiteral("Reveal in file manager"));
    connect(btnShowFolder, &QPushButton::clicked, this, &ViewerWindow::showInFolder);
    toolLayout->addWidget(btnShowFolder);

    auto* btnClose = new QPushButton(QStringLiteral("✕ Close"), this);
    btnClose->setToolTip(QStringLiteral("Close viewer (Esc)"));
    connect(btnClose, &QPushButton::clicked, this, &QWidget::close);
    toolLayout->addWidget(btnClose);

    toolLayout->addStretch(1);

    m_infoLabel = new QLabel(this);
    toolLayout->addWidget(m_infoLabel);

    mainLayout->addWidget(toolBar);

    // Canvas Scroll Area
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignCenter);

    m_imageLabel = new QLabel(m_scrollArea);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_scrollArea->setWidget(m_imageLabel);

    m_scrollArea->viewport()->installEventFilter(this);
    m_imageLabel->installEventFilter(this);

    mainLayout->addWidget(m_scrollArea, 1);

    applyTheme();
}

void ViewerWindow::setDarkMode(bool dark) {
    m_darkMode = dark;
    applyTheme();
    updateDisplay();
}

void ViewerWindow::applyTheme() {
    if (m_darkMode) {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    background-color: #0f121d;"
            "    color: #e2e8f0;"
            "}"
            "QPushButton {"
            "    background-color: #1e2436;"
            "    color: #e2e8f0;"
            "    border: 1px solid #334155;"
            "    border-radius: 4px;"
            "    padding: 5px 10px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #2e3852;"
            "}"
            "QScrollArea {"
            "    background-color: #0b0d14;"
            "    border: none;"
            "}"
            "QLabel {"
            "    color: #94a3b8;"
            "}"
        ));
    } else {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    background-color: #f8fafc;"
            "    color: #0f172a;"
            "}"
            "QPushButton {"
            "    background-color: #ffffff;"
            "    color: #0f172a;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 4px;"
            "    padding: 5px 10px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #f1f5f9;"
            "}"
            "QScrollArea {"
            "    background-color: #e2e8f0;"
            "    border: none;"
            "}"
            "QLabel {"
            "    color: #475569;"
            "}"
        ));
    }
}

void ViewerWindow::showImage(const QVector<ImageRecord>& items, int index) {
    m_items = items;
    m_currentIndex = qBound(0, index, m_items.size() - 1);
    m_fitMode = true;

    if (m_currentIndex >= 0 && m_currentIndex < m_items.size()) {
        const QString path = m_items[m_currentIndex].path;
        m_dragBtn->setFilePath(path);

        QString zipPath, innerPath;
        if (ZipUtils::isZipPath(path, &zipPath, &innerPath)) {
            const QByteArray bytes = ZipUtils::readZipEntryBytes(zipPath, innerPath);
            QBuffer buf;
            buf.setData(bytes);
            buf.open(QIODevice::ReadOnly);
            QImageReader reader(&buf);
            reader.setAutoTransform(true);
            m_currentImage = reader.read();
        } else {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            m_currentImage = reader.read();
        }
    }

    updateDisplay();
    show();
    raise();
    activateWindow();
}

void ViewerWindow::updateDisplay() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size() || m_currentImage.isNull()) {
        m_imageLabel->clear();
        m_infoLabel->clear();
        return;
    }

    const auto& item = m_items[m_currentIndex];
    m_dragBtn->setFilePath(item.path);
    m_btnFav->setText(item.isFavorite ? QStringLiteral("★ Favorited") : QStringLiteral("⭐ Favorite"));

    const QSize origSize = m_currentImage.size();
    QSize targetSize;

    if (m_fitMode) {
        const QSize availSize = m_scrollArea->viewport()->size() - QSize(20, 20);
        targetSize = origSize.scaled(availSize, Qt::KeepAspectRatio);
        m_zoomFactor = static_cast<double>(targetSize.width()) / origSize.width();
    } else {
        targetSize = origSize * m_zoomFactor;
    }

    const QPixmap pix = QPixmap::fromImage(
        m_currentImage.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
    );
    m_imageLabel->setPixmap(pix);
    m_imageLabel->resize(targetSize);

    const double mb = static_cast<double>(item.fileSize) / (1024.0 * 1024.0);
    m_infoLabel->setText(QStringLiteral("[%1 / %2] %3 (%4×%5, %6 MB) — %7%")
        .arg(m_currentIndex + 1)
        .arg(m_items.size())
        .arg(item.filename)
        .arg(origSize.width())
        .arg(origSize.height())
        .arg(mb, 0, 'f', 1)
        .arg(static_cast<int>(m_zoomFactor * 100.0))
    );
}

void ViewerWindow::zoomIn() {
    m_fitMode = false;
    m_zoomFactor = qMin(10.0, m_zoomFactor * 1.25);
    updateDisplay();
}

void ViewerWindow::zoomOut() {
    m_fitMode = false;
    m_zoomFactor = qMax(0.05, m_zoomFactor / 1.25);
    updateDisplay();
}

void ViewerWindow::zoomFit() {
    m_fitMode = true;
    updateDisplay();
}

void ViewerWindow::zoomOriginal() {
    m_fitMode = false;
    m_zoomFactor = 1.0;
    updateDisplay();
}

void ViewerWindow::showPrevious() {
    if (m_currentIndex > 0) {
        showImage(m_items, m_currentIndex - 1);
    }
}

void ViewerWindow::showNext() {
    if (m_currentIndex + 1 < m_items.size()) {
        showImage(m_items, m_currentIndex + 1);
    }
}

void ViewerWindow::copyFile() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    QString p = m_items[m_currentIndex].path;
    if (ZipUtils::isZipPath(p)) {
        p = ZipUtils::ensureExtracted(p);
    }
    if (p.isEmpty() || !QFile::exists(p)) return;

    auto* mime = new QMimeData();
    const QList<QUrl> urls = {QUrl::fromLocalFile(p)};
    mime->setUrls(urls);
    mime->setText(p);
    const QByteArray gnomeData = "copy\n" + urls[0].toString().toUtf8();
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnomeData);
    QApplication::clipboard()->setMimeData(mime);
}

void ViewerWindow::copyPath() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    QApplication::clipboard()->setText(m_items[m_currentIndex].path);
}

void ViewerWindow::copyImage() {
    if (m_currentImage.isNull()) return;
    QApplication::clipboard()->setImage(m_currentImage);
}

void ViewerWindow::openDefaultViewer() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    QString p = m_items[m_currentIndex].path;
    if (ZipUtils::isZipPath(p)) {
        p = ZipUtils::ensureExtracted(p);
    }
    if (!p.isEmpty() && QFile::exists(p)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(p));
    }
}

void ViewerWindow::toggleFavorite() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    auto& item = m_items[m_currentIndex];
    emit favoriteToggled(m_currentIndex, item.path);
    item.isFavorite = !item.isFavorite;
    m_btnFav->setText(item.isFavorite ? QStringLiteral("★ Favorited") : QStringLiteral("⭐ Favorite"));
}

void ViewerWindow::showInFolder() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    QString p = m_items[m_currentIndex].path;
    QString zipPath;
    if (ZipUtils::isZipPath(p, &zipPath)) {
        p = zipPath; // Highlight the zip archive
    }
    const QFileInfo fi(p);
    // Try dolphin, nautilus, fallback to xdg-open
    if (!QProcess::startDetached(QStringLiteral("dolphin"), QStringList{QStringLiteral("--select"), p})) {
        if (!QProcess::startDetached(QStringLiteral("nautilus"), QStringList{QStringLiteral("--select"), p})) {
            QProcess::startDetached(QStringLiteral("xdg-open"), QStringList{fi.absolutePath()});
        }
    }
}

void ViewerWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Up:
        showPrevious();
        return;
    case Qt::Key_Right:
    case Qt::Key_Down:
        showNext();
        return;
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Space:
        if (m_fitMode) {
            zoomOriginal();
        } else {
            zoomFit();
        }
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        return;
    case Qt::Key_Minus:
        zoomOut();
        return;
    case Qt::Key_0:
        zoomOriginal();
        return;
    case Qt::Key_F:
    case Qt::Key_S:
        toggleFavorite();
        return;
    case Qt::Key_C:
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            copyPath();
        } else if (event->modifiers() == Qt::ControlModifier) {
            copyFile();
        }
        return;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void ViewerWindow::wheelEvent(QWheelEvent* event) {
    if (event->angleDelta().y() > 0) {
        zoomIn();
    } else if (event->angleDelta().y() < 0) {
        zoomOut();
    }
    event->accept();
}

void ViewerWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_fitMode) {
        updateDisplay();
    }
}

bool ViewerWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_scrollArea->viewport() || watched == m_imageLabel) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                if (m_fitMode) {
                    zoomOriginal();
                } else {
                    zoomFit();
                }
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_panning = true;
                m_panStartPos = me->globalPos();
                m_scrollArea->viewport()->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (m_panning && (me->buttons() & Qt::LeftButton)) {
                const QPoint delta = me->globalPos() - m_panStartPos;
                m_panStartPos = me->globalPos();
                m_scrollArea->horizontalScrollBar()->setValue(m_scrollArea->horizontalScrollBar()->value() - delta.x());
                m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->value() - delta.y());
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && m_panning) {
                m_panning = false;
                m_scrollArea->viewport()->setCursor(m_fitMode ? Qt::ArrowCursor : Qt::OpenHandCursor);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

} // namespace OmniView
