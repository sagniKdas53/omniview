#include "ViewerWindow.h"
#include "ColorUtils.h"
#include "FileActions.h"
#include "ZipUtils.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QDrag>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QTimer>
#include <QtMath>

#include <cmath>

namespace OmniView {

namespace {

QPoint globalPosOf(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition().toPoint();
#else
    return e->globalPos();
#endif
}

} // namespace

DragButton::DragButton(const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Click and drag directly to Discord, Slack, or browser to attach!"));
}

void DragButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        m_pressed = true;
    }
    QPushButton::mousePressEvent(event);
}

void DragButton::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_pressed = false;
    }
    QPushButton::mouseReleaseEvent(event);
}

void DragButton::mouseMoveEvent(QMouseEvent* event) {
    if ((event->buttons() & Qt::LeftButton) && m_pressed) {
        const int dist = (event->pos() - m_dragStartPos).manhattanLength();
        if (dist >= QApplication::startDragDistance()) {
            m_pressed = false;
            setDown(false);
            const QStringList localFiles = FileActions::resolveLocalFiles(m_filePaths);
            if (!localFiles.isEmpty()) {
                auto* drag = new QDrag(this);
                drag->setMimeData(FileActions::createFileMimeData(localFiles));
                // Reduced, EXIF-aware decode just for the 120 px badge.
                const QImage small = ColorUtils::loadImage(m_filePaths.first(), QSize(240, 240));
                const QPixmap badge = FileActions::makeDragBadge(
                    small.isNull() ? QPixmap()
                                   : QPixmap::fromImage(small.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation)),
                    static_cast<int>(localFiles.size()));
                if (!badge.isNull()) {
                    drag->setPixmap(badge);
                    drag->setHotSpot(QPoint(badge.width() / 2, badge.height() / 2));
                }
                drag->exec(Qt::CopyAction);
            }
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
    toolLayout->setSpacing(6);

    auto* btnPrev = new QPushButton(QStringLiteral("◀ Previous (Left)"), this);
    btnPrev->setToolTip(QStringLiteral("Previous image (Left Arrow)"));
    connect(btnPrev, &QPushButton::clicked, this, &ViewerWindow::showPrevious);
    toolLayout->addWidget(btnPrev);

    auto* btnNext = new QPushButton(QStringLiteral("Next (Right) ▶"), this);
    btnNext->setToolTip(QStringLiteral("Next image (Right Arrow)"));
    connect(btnNext, &QPushButton::clicked, this, &ViewerWindow::showNext);
    toolLayout->addWidget(btnNext);

    toolLayout->addSpacing(8);

    auto* btnFit = new QPushButton(QStringLiteral("Fit to Window (Space)"), this);
    btnFit->setToolTip(QStringLiteral("Fit image inside window (Space bar)"));
    connect(btnFit, &QPushButton::clicked, this, &ViewerWindow::zoomFit);
    toolLayout->addWidget(btnFit);

    auto* btn100 = new QPushButton(QStringLiteral("1:1 (100%)"), this);
    btn100->setToolTip(QStringLiteral("View at native resolution"));
    connect(btn100, &QPushButton::clicked, this, &ViewerWindow::zoomOriginal);
    toolLayout->addWidget(btn100);

    auto* btnZoomOut = new QPushButton(QStringLiteral("🔍 −"), this);
    btnZoomOut->setToolTip(QStringLiteral("Zoom out"));
    connect(btnZoomOut, &QPushButton::clicked, this, &ViewerWindow::zoomOut);
    toolLayout->addWidget(btnZoomOut);

    auto* btnZoomIn = new QPushButton(QStringLiteral("🔍 +"), this);
    btnZoomIn->setToolTip(QStringLiteral("Zoom in"));
    connect(btnZoomIn, &QPushButton::clicked, this, &ViewerWindow::zoomIn);
    toolLayout->addWidget(btnZoomIn);

    toolLayout->addSpacing(8);

    // Direct Drag-and-Drop Button (Indigo pill)
    m_dragBtn = new DragButton(QStringLiteral("📤 Drag to Attach"), this);
    m_dragBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "    background-color: #4f46e5;"
        "    color: #ffffff;"
        "    font-weight: bold;"
        "    padding: 5px 12px;"
        "    border-radius: 5px;"
        "    border: none;"
        "}"
        "QPushButton:hover {"
        "    background-color: #4338ca;"
        "}"
    ));
    toolLayout->addWidget(m_dragBtn);

    m_btnFav = new QPushButton(QStringLiteral("⭐ Favorite"), this);
    m_btnFav->setToolTip(QStringLiteral("Toggle favorite (F)"));
    connect(m_btnFav, &QPushButton::clicked, this, &ViewerWindow::toggleFavorite);
    toolLayout->addWidget(m_btnFav);

    auto* btnCopyImg = new QPushButton(QStringLiteral("🖼️ Copy Image"), this);
    btnCopyImg->setToolTip(QStringLiteral("Copy image bitmap to clipboard"));
    connect(btnCopyImg, &QPushButton::clicked, this, &ViewerWindow::copyImage);
    toolLayout->addWidget(btnCopyImg);

    auto* btnCopyPath = new QPushButton(QStringLiteral("📋 Copy Path"), this);
    btnCopyPath->setToolTip(QStringLiteral("Copy file path to clipboard (Ctrl+Shift+C)"));
    connect(btnCopyPath, &QPushButton::clicked, this, &ViewerWindow::copyPath);
    toolLayout->addWidget(btnCopyPath);

    auto* btnCopyFile = new QPushButton(QStringLiteral("📁 Copy File (Attach)"), this);
    btnCopyFile->setToolTip(QStringLiteral("Copy file for pasting into chat or file manager (Ctrl+C)"));
    connect(btnCopyFile, &QPushButton::clicked, this, &ViewerWindow::copyFile);
    toolLayout->addWidget(btnCopyFile);

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
    m_infoLabel->setStyleSheet(QStringLiteral("color: #38bdf8; font-weight: bold; font-size: 11px;"));
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

    // Keyboard navigation belongs to the window: a focused toolbar button or
    // the scroll area would otherwise swallow arrows and Space.
    for (auto* button : findChildren<QPushButton*>()) {
        button->setFocusPolicy(Qt::NoFocus);
    }
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    setFocusPolicy(Qt::StrongFocus);

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
            "    font-family: \"Noto Sans\", \"Ubuntu\", \"DejaVu Sans\", \"Symbola\", \"Noto Color Emoji\", \"Segoe UI Emoji\", sans-serif;"
            "    background-color: #161922;"
            "    color: #dce1eb;"
            "}"
            "QPushButton {"
            "    background-color: #1e2438;"
            "    color: #dce1eb;"
            "    border: 1px solid #2d3241;"
            "    border-radius: 5px;"
            "    padding: 5px 10px;"
            "    font-size: 11px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #283046;"
            "    border-color: #4f46e5;"
            "}"
            "QScrollArea {"
            "    background-color: #0b0f19;"
            "    border: none;"
            "}"
            "QLabel {"
            "    color: #94a3b8;"
            "}"
        ));
    } else {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    font-family: \"Noto Sans\", \"Ubuntu\", \"DejaVu Sans\", \"Symbola\", \"Noto Color Emoji\", \"Segoe UI Emoji\", sans-serif;"
            "    background-color: #f8fafc;"
            "    color: #0f172a;"
            "}"
            "QPushButton {"
            "    background-color: #ffffff;"
            "    color: #0f172a;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 5px;"
            "    padding: 5px 10px;"
            "    font-size: 11px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #f1f5f9;"
            "    border-color: #4f46e5;"
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
    m_currentIndex = m_items.isEmpty() ? -1 : qBound(0, index, static_cast<int>(m_items.size()) - 1);
    m_fitMode = true;
    loadCurrentImage();

    // show() keeps a minimized window minimized; restore it explicitly.
    if (windowState() & Qt::WindowMinimized) {
        setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    }
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    updateDisplay();
}

void ViewerWindow::loadCurrentImage() {
    m_currentImage = QImage();
    m_originalSize = QSize();
    m_loadError.clear();
    m_wheelAccumulator = 0;
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) {
        m_dragBtn->setFilePath(QString());
        return;
    }
    const QString path = m_items.at(m_currentIndex).path;
    m_dragBtn->setFilePath(path);
    // Bounded, EXIF-aware decode: anything larger than the largest view we
    // ever render is decoded at reduced size where the format allows it.
    m_currentImage = ColorUtils::loadImage(
        path, QSize(MAX_VIEW_DIMENSION, MAX_VIEW_DIMENSION), &m_originalSize, &m_loadError);
    if (m_currentImage.isNull() && m_loadError.isEmpty()) {
        m_loadError = QStringLiteral("Could not load image");
    }
}

void ViewerWindow::updateDisplay() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) {
        m_imageLabel->clear();
        m_imageLabel->setFixedSize(1, 1);
        m_infoLabel->clear();
        setWindowTitle(QStringLiteral("OmniView — Image Viewer"));
        return;
    }

    const ImageRecord& item = m_items.at(m_currentIndex);
    m_btnFav->setText(item.isFavorite ? QStringLiteral("★ Favorited") : QStringLiteral("⭐ Favorite"));
    setWindowTitle(QStringLiteral("%1 — OmniView Viewer (%2 / %3)")
        .arg(item.filename, QString::number(m_currentIndex + 1), QString::number(m_items.size())));

    if (m_currentImage.isNull()) {
        m_imageLabel->setFixedSize(m_scrollArea->viewport()->size());
        m_imageLabel->setText(QStringLiteral("⚠ %1").arg(m_loadError));
        m_infoLabel->setText(QStringLiteral("%1 / %2 — cannot display: %3")
            .arg(QString::number(m_currentIndex + 1), QString::number(m_items.size()), m_loadError));
        return;
    }

    const QSize decodedSize = m_currentImage.size();
    const QSize origSize = m_originalSize.isValid() ? m_originalSize : decodedSize;
    QSize targetSize;

    if (m_fitMode) {
        QSize availSize = m_scrollArea->viewport()->size() - QSize(24, 24);
        if (availSize.width() < 50 || availSize.height() < 50) {
            // Not laid out yet: estimate from the window.
            availSize = size() - QSize(40, 100);
        }
        availSize = availSize.expandedTo(QSize(1, 1));

        targetSize = origSize.scaled(availSize, Qt::KeepAspectRatio);
        // Small images: upscale at most 3x when fitting.
        if (targetSize.width() > origSize.width() * 3) {
            targetSize = origSize * 3;
        }
        m_zoomFactor = static_cast<double>(targetSize.width()) / origSize.width();
    } else {
        targetSize = origSize * m_zoomFactor;
    }

    targetSize = targetSize.expandedTo(QSize(1, 1));
    if (targetSize.width() > MAX_VIEW_DIMENSION || targetSize.height() > MAX_VIEW_DIMENSION) {
        targetSize = targetSize.scaled(MAX_VIEW_DIMENSION, MAX_VIEW_DIMENSION, Qt::KeepAspectRatio);
    }

    // Render at device resolution so HiDPI screens get crisp pixels, keeping
    // the backing store within MAX_VIEW_DIMENSION device pixels.
    QSize deviceSize = (QSizeF(targetSize) * devicePixelRatioF()).toSize().expandedTo(QSize(1, 1));
    if (deviceSize.width() > MAX_VIEW_DIMENSION || deviceSize.height() > MAX_VIEW_DIMENSION) {
        deviceSize = deviceSize.scaled(MAX_VIEW_DIMENSION, MAX_VIEW_DIMENSION, Qt::KeepAspectRatio);
    }
    // deviceSize already has targetSize's aspect ratio; scaling to it exactly
    // avoids KeepAspectRatio rounding the width off by a pixel or two (Qt 6.11
    // then rejects the resulting sub-1 device pixel ratio).
    QPixmap pix = QPixmap::fromImage(
        m_currentImage.scaled(deviceSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    pix.setDevicePixelRatio(static_cast<qreal>(deviceSize.width()) / targetSize.width());
    m_imageLabel->setPixmap(pix);
    m_imageLabel->setFixedSize(targetSize);

    const double mb = static_cast<double>(item.fileSize) / (1024.0 * 1024.0);
    const double shownZoom = static_cast<double>(targetSize.width()) / origSize.width();
    m_infoLabel->setText(QStringLiteral("%1 / %2 — %3×%4 (%5 MB, %6%)")
        .arg(QString::number(m_currentIndex + 1), QString::number(m_items.size()),
             QString::number(origSize.width()), QString::number(origSize.height()),
             QString::number(mb, 'f', 1), QString::number(qRound(shownZoom * 100.0))));
}

void ViewerWindow::zoomBy(double factor) {
    if (m_currentImage.isNull()) return;
    m_fitMode = false;
    m_zoomFactor = qBound(MIN_ZOOM, m_zoomFactor * factor, MAX_ZOOM);
    updateDisplay();
}

void ViewerWindow::zoomIn() {
    zoomBy(1.25);
}

void ViewerWindow::zoomOut() {
    zoomBy(1.0 / 1.25);
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
        --m_currentIndex;
        m_fitMode = true;
        loadCurrentImage();
        updateDisplay();
    }
}

void ViewerWindow::showNext() {
    if (m_currentIndex + 1 < m_items.size()) {
        ++m_currentIndex;
        m_fitMode = true;
        loadCurrentImage();
        updateDisplay();
    }
}

void ViewerWindow::copyFile() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    FileActions::copyFilesToClipboard({m_items.at(m_currentIndex).path});
}

void ViewerWindow::copyPath() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    QApplication::clipboard()->setText(m_items.at(m_currentIndex).path);
}

void ViewerWindow::copyImage() {
    if (m_currentImage.isNull()) return;
    QApplication::clipboard()->setImage(m_currentImage);
}

void ViewerWindow::openDefaultViewer() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    FileActions::openWithDefaultApp(m_items.at(m_currentIndex).path);
}

void ViewerWindow::toggleFavorite() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    emit favoriteToggled(m_currentIndex, m_items.at(m_currentIndex).path);
}

void ViewerWindow::setFavoriteState(const QString& path, bool favorite) {
    for (auto& item : m_items) {
        if (item.path == path) item.isFavorite = favorite;
    }
    if (m_currentIndex >= 0 && m_currentIndex < m_items.size() && m_items.at(m_currentIndex).path == path) {
        m_btnFav->setText(favorite ? QStringLiteral("★ Favorited") : QStringLiteral("⭐ Favorite"));
    }
}

void ViewerWindow::showInFolder() {
    if (m_currentIndex < 0 || m_currentIndex >= m_items.size()) return;
    FileActions::revealInFileManager(m_items.at(m_currentIndex).path);
}

void ViewerWindow::keyPressEvent(QKeyEvent* event) {
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
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
        if (mods == Qt::NoModifier) {
            toggleFavorite();
            return;
        }
        break;
    case Qt::Key_C:
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
            copyPath();
            return;
        }
        if (mods == Qt::ControlModifier) {
            copyFile();
            return;
        }
        break;
    default:
        break;
    }
    QMainWindow::keyPressEvent(event);
}

void ViewerWindow::wheelEvent(QWheelEvent* event) {
    // Reached only outside the scroll area; the viewport filter handles the rest.
    m_wheelAccumulator += event->angleDelta().y();
    const int steps = m_wheelAccumulator / 120;
    if (steps != 0) {
        m_wheelAccumulator -= steps * 120;
        zoomBy(std::pow(1.25, steps));
    }
    event->accept();
}

void ViewerWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (m_fitMode) {
        QTimer::singleShot(0, this, [this]() {
            if (m_fitMode) {
                updateDisplay();
            }
        });
    }
}

void ViewerWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    if (!event->spontaneous()) {
        // Closed (not minimized): release the decoded image and pixmap.
        m_currentImage = QImage();
        m_imageLabel->clear();
        m_currentIndex = -1;
        m_items.clear();
    }
}

void ViewerWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_fitMode) {
        updateDisplay();
    }
}

bool ViewerWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_scrollArea->viewport() || watched == m_imageLabel) {
        if (event->type() == QEvent::Wheel) {
            // Zoom instead of letting the scroll area scroll.
            auto* we = static_cast<QWheelEvent*>(event);
            m_wheelAccumulator += we->angleDelta().y();
            const int steps = m_wheelAccumulator / 120;
            if (steps != 0) {
                m_wheelAccumulator -= steps * 120;
                zoomBy(std::pow(1.25, steps));
            }
            we->accept();
            return true;
        }
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
                m_panStartPos = globalPosOf(me);
                m_scrollArea->viewport()->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (m_panning && (me->buttons() & Qt::LeftButton)) {
                const QPoint pos = globalPosOf(me);
                const QPoint delta = pos - m_panStartPos;
                m_panStartPos = pos;
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
