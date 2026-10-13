#include "MainWindow.h"
#include "Config.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QApplication>

#include <utility>

namespace OmniView {

MainWindow::MainWindow(const QString& initialDir, QWidget* parent)
    : QMainWindow(parent)
    , m_currentRootDir(QDir(initialDir.isEmpty() ? Config::defaultRootDir() : initialDir).absolutePath())
    , m_thumbMgr(m_db.dbPath())
{
    Config::ensureDirectories();
    if (!m_db.initSchema()) {
        m_dbError = m_db.lastError().isEmpty() ? QStringLiteral("cannot open %1").arg(m_db.dbPath())
                                               : m_db.lastError();
    }

    setWindowTitle(Config::appTitle());
    resize(1380, 880);

    m_viewerWindow = new ViewerWindow(this);
    connect(m_viewerWindow, &ViewerWindow::favoriteToggled, this,
            [this](int /*idx*/, const QString& path) { toggleFavoriteForPath(path); });

    // Live updates: recursive, diffed directory watching with debounced rescans.
    m_watcher = new LibraryWatcher(this);
    connect(m_watcher, &LibraryWatcher::rescanRequested, this, &MainWindow::onWatcherRescanRequested);

    // Search debounce timer
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(300);
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::refreshGallery);

    // Thumbnail generation signals
    connect(&m_thumbMgr, &ThumbnailManager::thumbnailReady, this, &MainWindow::onThumbnailReady);

    setupUi();
    applyTheme();
    refreshFolderList();
    refreshGallery();
    startScan();
}

MainWindow::~MainWindow() {
    // Stop background work before members go away: queued thumbnails are
    // dropped (not drained) and a running scan is cancelled; the Scanner child
    // waits for its thread when it is deleted.
    m_thumbMgr.cancelPending();
    if (m_scanner) {
        m_scanner->cancel();
    }
}

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top Bar
    mainLayout->addWidget(createTopBar());

    // Splitter: Sidebar + Gallery
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(2);

    splitter->addWidget(createSidebar());

    // Gallery Container
    auto* galleryContainer = new QWidget(this);
    auto* galleryLayout = new QVBoxLayout(galleryContainer);
    galleryLayout->setContentsMargins(8, 8, 8, 8);
    galleryLayout->setSpacing(6);

    m_model = new GalleryModel(this);
    m_delegate = new GalleryDelegate(&m_thumbMgr, this);
    m_delegate->setCardSize(Config::DEFAULT_GRID_SIZE);

    m_view = new GalleryView(&m_thumbMgr, this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(m_delegate);

    connect(m_view, &GalleryView::openViewerRequested, this, &MainWindow::onOpenViewer);
    connect(m_view, &GalleryView::selectionCountChanged, this, &MainWindow::onSelectionCountChanged);
    connect(m_view, &GalleryView::favoriteToggled, this, &MainWindow::onFavoriteToggled);
    connect(m_view, &GalleryView::filterToSubfolderRequested, this, [this](const QString& sub) {
        m_filter.selectSubfolder(sub);
        if (!m_folderSearch->text().isEmpty()) {
            m_folderSearch->clear(); // also refreshes the sidebar
        } else {
            refreshFolderList();
        }
        refreshGallery();
    });

    connect(m_view, &GalleryView::findSimilarRequested, this, &MainWindow::onFindSimilarRequested);

    // Similarity Search Banner
    m_similarityBanner = new QFrame(this);
    m_similarityBanner->setObjectName(QStringLiteral("similarityBanner"));
    auto* simLayout = new QHBoxLayout(m_similarityBanner);
    simLayout->setContentsMargins(12, 6, 12, 6);
    simLayout->setSpacing(8);

    m_lblSimilarity = new QLabel(m_similarityBanner);
    m_lblSimilarity->setStyleSheet(QStringLiteral("font-weight: bold; font-size: 13px; color: #a5b4fc;"));
    simLayout->addWidget(m_lblSimilarity, 1);

    m_btnClearSimilarity = new QPushButton(QStringLiteral("✕ Clear Similarity Search"), m_similarityBanner);
    m_btnClearSimilarity->setStyleSheet(QStringLiteral("background-color: #4338ca; color: #ffffff; padding: 4px 10px; border-radius: 4px; font-weight: bold;"));
    connect(m_btnClearSimilarity, &QPushButton::clicked, this, &MainWindow::onClearSimilarityClicked);
    simLayout->addWidget(m_btnClearSimilarity);

    m_similarityBanner->hide();
    galleryLayout->addWidget(m_similarityBanner);

    galleryLayout->addWidget(m_view, 1);

    // Batch Action Toolbar (floating bottom panel)
    m_batchBar = qobject_cast<QFrame*>(createBatchToolbar());
    m_batchBar->hide();
    galleryLayout->addWidget(m_batchBar);

    splitter->addWidget(galleryContainer);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({240, 1140});

    mainLayout->addWidget(splitter, 1);
}

QWidget* MainWindow::createTopBar() {
    auto* bar = new QWidget(this);
    auto* barLayout = new QVBoxLayout(bar);
    barLayout->setContentsMargins(12, 6, 12, 6);
    barLayout->setSpacing(6);

    // Row 1: Folder, Browse, Search, Sort, Rescan, Batch Select, Theme
    auto* row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(8);

    auto* lblFolder = new QLabel(QStringLiteral("Folder:"), this);
    lblFolder->setStyleSheet(QStringLiteral("font-weight: bold;"));
    row1->addWidget(lblFolder);

    m_folderPathEdit = new QLineEdit(this);
    m_folderPathEdit->setText(m_currentRootDir);
    m_folderPathEdit->setPlaceholderText(QStringLiteral("Enter directory path..."));
    m_folderPathEdit->setMinimumWidth(220);
    connect(m_folderPathEdit, &QLineEdit::returnPressed, this, [this]() {
        const QString text = m_folderPathEdit->text().trimmed();
        if (!text.isEmpty() && QDir(text).exists()) {
            changeRootDir(text);
        } else {
            m_folderPathEdit->setText(m_currentRootDir);
        }
    });
    row1->addWidget(m_folderPathEdit, 1);

    m_btnBrowse = new QPushButton(QStringLiteral("📂 Browse"), this);
    m_btnBrowse->setToolTip(QStringLiteral("Select directory"));
    connect(m_btnBrowse, &QPushButton::clicked, this, &MainWindow::onChangeFolderClicked);
    row1->addWidget(m_btnBrowse);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("🔍 Search files and subfolders..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumWidth(180);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    row1->addWidget(m_searchEdit, 1);

    // Sort
    auto* lblSort = new QLabel(QStringLiteral("Sort:"), this);
    lblSort->setStyleSheet(QStringLiteral("font-weight: bold;"));
    row1->addWidget(lblSort);

    m_sortCombo = new QComboBox(this);
    m_sortCombo->addItem(QStringLiteral("📅 Newest First"), QStringLiteral("mtime_desc"));
    m_sortCombo->addItem(QStringLiteral("📅 Oldest First"), QStringLiteral("mtime_asc"));
    m_sortCombo->addItem(QStringLiteral("📦 Largest Size"), QStringLiteral("size_desc"));
    m_sortCombo->addItem(QStringLiteral("📦 Smallest Size"), QStringLiteral("size_asc"));
    m_sortCombo->addItem(QStringLiteral("🔤 Filename A-Z"), QStringLiteral("name_asc"));
    m_sortCombo->addItem(QStringLiteral("🎲 Random Shuffle"), QStringLiteral("random"));
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSortChanged);
    row1->addWidget(m_sortCombo);

    m_btnRescan = new QPushButton(QStringLiteral("🔄 Rescan"), this);
    m_btnRescan->setToolTip(QStringLiteral("Rescan current directory"));
    connect(m_btnRescan, &QPushButton::clicked, this, &MainWindow::onRescanClicked);
    row1->addWidget(m_btnRescan);

    m_btnSelectMode = new QPushButton(QStringLiteral("☑ Batch Select"), this);
    m_btnSelectMode->setCheckable(true);
    connect(m_btnSelectMode, &QPushButton::clicked, this, &MainWindow::onSelectModeToggled);
    row1->addWidget(m_btnSelectMode);

    m_btnTheme = new QPushButton(QStringLiteral("🌙 Dark"), this);
    m_btnTheme->setToolTip(QStringLiteral("Toggle Dark / Light mode"));
    connect(m_btnTheme, &QPushButton::clicked, this, &MainWindow::onThemeToggled);
    row1->addWidget(m_btnTheme);

    barLayout->addLayout(row1);

    // Row 2: Colors pills, Aspect pills, Size slider, Total stats
    auto* row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(6);

    auto* lblColors = new QLabel(QStringLiteral("Colors:"), this);
    lblColors->setStyleSheet(QStringLiteral("font-weight: bold;"));
    row2->addWidget(lblColors);

    m_colorButtons.clear();
    for (const auto& item : ColorUtils::palette()) {
        auto* btn = new QPushButton(item.label, this);
        btn->setProperty("colorId", item.id);
        btn->setProperty("hexColor", item.hexColor);
        btn->setProperty("label", item.label);
        btn->setCursor(Qt::PointingHandCursor);
        const QString cid = item.id;
        connect(btn, &QPushButton::clicked, this, [this, cid]() {
            onColorPillClicked(cid);
        });
        row2->addWidget(btn);
        m_colorButtons.append(btn);
    }

    row2->addSpacing(10);

    auto* lblAspect = new QLabel(QStringLiteral("Aspect:"), this);
    lblAspect->setStyleSheet(QStringLiteral("font-weight: bold;"));
    row2->addWidget(lblAspect);

    m_aspectButtons.clear();
    const QVector<QPair<QString, QString>> aspects = {
        {QStringLiteral("all"), QStringLiteral("All")},
        {QStringLiteral("portrait"), QStringLiteral("📱 Portrait")},
        {QStringLiteral("landscape"), QStringLiteral("🖥️ Landscape")},
        {QStringLiteral("square"), QStringLiteral("🔲 Square")}
    };
    for (const auto& pair : aspects) {
        auto* btn = new QPushButton(pair.second, this);
        btn->setProperty("aspectId", pair.first);
        btn->setCursor(Qt::PointingHandCursor);
        const QString aid = pair.first;
        connect(btn, &QPushButton::clicked, this, [this, aid]() {
            onAspectPillClicked(aid);
        });
        row2->addWidget(btn);
        m_aspectButtons.append(btn);
    }

    row2->addSpacing(10);

    auto* lblSize = new QLabel(QStringLiteral("Size:"), this);
    lblSize->setStyleSheet(QStringLiteral("font-weight: bold;"));
    row2->addWidget(lblSize);

    m_cardSizeSlider = new QSlider(Qt::Horizontal, this);
    m_cardSizeSlider->setRange(Config::MIN_GRID_SIZE, Config::MAX_GRID_SIZE);
    m_cardSizeSlider->setValue(Config::DEFAULT_GRID_SIZE);
    m_cardSizeSlider->setFixedWidth(90);
    connect(m_cardSizeSlider, &QSlider::valueChanged, this, &MainWindow::onCardSizeChanged);
    row2->addWidget(m_cardSizeSlider);

    row2->addStretch(1);

    m_lblStats = new QLabel(QStringLiteral("Scanning..."), this);
    m_lblStats->setStyleSheet(QStringLiteral("font-size: 11px;"));
    row2->addWidget(m_lblStats);

    barLayout->addLayout(row2);

    updateColorPillStyles();
    updateAspectPillStyles();

    return bar;
}

QWidget* MainWindow::createSidebar() {
    auto* sidebar = new QWidget(this);
    sidebar->setMinimumWidth(200);
    sidebar->setMaximumWidth(360);

    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("FOLDERS"), sidebar);
    QFont f = title->font();
    f.setBold(true);
    f.setPointSize(9);
    title->setFont(f);
    layout->addWidget(title);

    m_folderSearch = new QLineEdit(sidebar);
    m_folderSearch->setPlaceholderText(QStringLiteral("Filter folders..."));
    m_folderSearch->setClearButtonEnabled(true);
    connect(m_folderSearch, &QLineEdit::textChanged, this, &MainWindow::onFolderSearchChanged);
    layout->addWidget(m_folderSearch);

    m_folderList = new QListWidget(sidebar);
    connect(m_folderList, &QListWidget::itemClicked, this, &MainWindow::onFolderItemClicked);
    layout->addWidget(m_folderList, 1);

    return sidebar;
}

QWidget* MainWindow::createBatchToolbar() {
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("batchToolbar"));
    frame->setFrameShape(QFrame::StyledPanel);

    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(12);

    m_lblBatchCount = new QLabel(QStringLiteral("📦 0 item(s) selected"), frame);
    QFont f = m_lblBatchCount->font();
    f.setBold(true);
    f.setPointSize(10);
    m_lblBatchCount->setFont(f);
    m_lblBatchCount->setStyleSheet(QStringLiteral("color: #38bdf8;"));
    layout->addWidget(m_lblBatchCount);

    // Direct Batch Drag Button!
    m_btnBatchDrag = new DragButton(QStringLiteral("📤 Drag to Attach"), frame);
    m_btnBatchDrag->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "    background-color: #4f46e5;"
        "    color: #ffffff;"
        "    font-weight: bold;"
        "    padding: 6px 14px;"
        "    border-radius: 5px;"
        "    border: none;"
        "}"
        "QPushButton:hover {"
        "    background-color: #4338ca;"
        "}"
    ));
    layout->addWidget(m_btnBatchDrag);

    m_btnCopyFiles = new QPushButton(QStringLiteral("📁 Copy Files (Attach)"), frame);
    connect(m_btnCopyFiles, &QPushButton::clicked, m_view, &GalleryView::copySelectedFiles);
    layout->addWidget(m_btnCopyFiles);

    m_btnCopyPaths = new QPushButton(QStringLiteral("📋 Copy Paths"), frame);
    connect(m_btnCopyPaths, &QPushButton::clicked, m_view, &GalleryView::copySelectedPaths);
    layout->addWidget(m_btnCopyPaths);

    auto* btnSelectAll = new QPushButton(QStringLiteral("✓ Select All"), frame);
    connect(btnSelectAll, &QPushButton::clicked, m_view, &GalleryView::selectAllItems);
    layout->addWidget(btnSelectAll);

    auto* btnInvert = new QPushButton(QStringLiteral("🔄 Invert"), frame);
    connect(btnInvert, &QPushButton::clicked, m_view, &GalleryView::invertSelection);
    layout->addWidget(btnInvert);

    auto* btnClear = new QPushButton(QStringLiteral("Deselect All"), frame);
    connect(btnClear, &QPushButton::clicked, m_view, &GalleryView::clearAllSelection);
    layout->addWidget(btnClear);

    layout->addStretch(1);

    auto* btnExit = new QPushButton(QStringLiteral("✕ Exit Select Mode"), frame);
    connect(btnExit, &QPushButton::clicked, this, [this]() {
        m_btnSelectMode->setChecked(false);
        onSelectModeToggled();
    });
    layout->addWidget(btnExit);

    return frame;
}

void MainWindow::updateColorPillStyles() {
    for (auto* btn : m_colorButtons) {
        const QString colorId = btn->property("colorId").toString();
        const QString hex = btn->property("hexColor").toString();
        const QString lbl = btn->property("label").toString();
        const bool isSel = (m_filter.colorName() == colorId);

        btn->setText(isSel ? QStringLiteral("• %1 •").arg(lbl) : lbl);

        const QString textCol = (colorId == QStringLiteral("white")) ? QStringLiteral("#0f172a") : QStringLiteral("#ffffff");
        const QString border = isSel
            ? QStringLiteral("border: 2px solid #ffffff; font-weight: bold;")
            : QStringLiteral("border: 1px solid rgba(255, 255, 255, 0.25); font-weight: normal;");

        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "    background-color: %1;"
            "    color: %2;"
            "    %3"
            "    border-radius: 4px;"
            "    padding: 2px 7px;"
            "    font-size: 11px;"
            "}"
            "QPushButton:hover {"
            "    border: 2px solid #00d2ff;"
            "}"
        ).arg(hex, textCol, border));
    }
}

void MainWindow::updateAspectPillStyles() {
    for (auto* btn : m_aspectButtons) {
        const QString aspectId = btn->property("aspectId").toString();
        const bool isSel = (m_filter.aspectType() == aspectId);

        if (isSel) {
            btn->setStyleSheet(QStringLiteral(
                "QPushButton {"
                "    background-color: #4f46e5;"
                "    color: #ffffff;"
                "    border: 1px solid #6366f1;"
                "    border-radius: 4px;"
                "    padding: 3px 9px;"
                "    font-size: 11px;"
                "    font-weight: bold;"
                "}"
            ));
        } else {
            const QString bg = m_darkMode ? QStringLiteral("#1e2438") : QStringLiteral("#f1f5f9");
            const QString text = m_darkMode ? QStringLiteral("#dce1eb") : QStringLiteral("#334155");
            const QString bColor = m_darkMode ? QStringLiteral("#2d3241") : QStringLiteral("#cbd5e1");
            const QString hoverBg = m_darkMode ? QStringLiteral("#283046") : QStringLiteral("#e2e8f0");
            btn->setStyleSheet(QStringLiteral(
                "QPushButton {"
                "    background-color: %1;"
                "    color: %2;"
                "    border: 1px solid %3;"
                "    border-radius: 4px;"
                "    padding: 3px 9px;"
                "    font-size: 11px;"
                "    font-weight: normal;"
                "}"
                "QPushButton:hover {"
                "    background-color: %4;"
                "    border-color: #4f46e5;"
                "}"
            ).arg(bg, text, bColor, hoverBg));
        }
    }
}

void MainWindow::applyTheme() {
    if (m_darkMode) {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    font-family: \"Noto Sans\", \"Ubuntu\", \"DejaVu Sans\", \"Symbola\", \"Noto Color Emoji\", \"Segoe UI Emoji\", sans-serif;"
            "}"
            "QMainWindow {"
            "    background-color: #161922;"
            "    color: #dce1eb;"
            "}"
            "QSplitter::handle {"
            "    background-color: #2d3241;"
            "}"
            "QLineEdit, QComboBox {"
            "    background-color: #1e2438;"
            "    border: 1px solid #2d3241;"
            "    border-radius: 5px;"
            "    color: #dce1eb;"
            "    padding: 5px 8px;"
            "}"
            "QLineEdit:focus, QComboBox:focus {"
            "    border-color: #4f46e5;"
            "}"
            "QComboBox QAbstractItemView {"
            "    background-color: #1e2438;"
            "    color: #dce1eb;"
            "    selection-background-color: #4f46e5;"
            "    selection-color: #ffffff;"
            "    border: 1px solid #2d3241;"
            "    border-radius: 5px;"
            "}"
            "QPushButton {"
            "    background-color: #1e2438;"
            "    border: 1px solid #2d3241;"
            "    border-radius: 5px;"
            "    color: #dce1eb;"
            "    padding: 5px 12px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #283046;"
            "    border-color: #4f46e5;"
            "}"
            "QPushButton:checked {"
            "    background-color: #4f46e5;"
            "    color: #ffffff;"
            "    border-color: #6366f1;"
            "}"
            "QListWidget {"
            "    background-color: #161922;"
            "    border: 1px solid #2d3241;"
            "    border-radius: 6px;"
            "    color: #dce1eb;"
            "    outline: none;"
            "    padding: 4px;"
            "}"
            "QListWidget::item {"
            "    padding: 6px 10px;"
            "    border-radius: 5px;"
            "    margin: 1px 2px;"
            "}"
            "QListWidget::item:hover {"
            "    background-color: #222838;"
            "}"
            "QListWidget::item:selected {"
            "    background-color: #1e293b;"
            "    color: #38bdf8;"
            "    font-weight: bold;"
            "}"
            "QFrame {"
            "    background-color: #161922;"
            "    border: 1px solid #2d3241;"
            "    border-radius: 6px;"
            "}"
            "QFrame#batchToolbar {"
            "    background-color: #0f172a;"
            "    border: 1px solid #1e3a8a;"
            "    border-radius: 8px;"
            "}"
            "QFrame#similarityBanner {"
            "    background-color: #1e1b4b;"
            "    border: 1px solid #4f46e5;"
            "    border-radius: 6px;"
            "}"
            "QLabel {"
            "    background-color: transparent;"
            "    color: #dce1eb;"
            "}"
            "QScrollBar:vertical {"
            "    background: #161922;"
            "    width: 8px;"
            "    margin: 0px;"
            "}"
            "QScrollBar::handle:vertical {"
            "    background: #334155;"
            "    min-height: 20px;"
            "    border-radius: 4px;"
            "}"
            "QScrollBar::handle:vertical:hover {"
            "    background: #4f46e5;"
            "}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
            "    height: 0px;"
            "}"
            "QScrollBar:horizontal {"
            "    background: #161922;"
            "    height: 8px;"
            "    margin: 0px;"
            "}"
            "QScrollBar::handle:horizontal {"
            "    background: #334155;"
            "    min-width: 20px;"
            "    border-radius: 4px;"
            "}"
            "QScrollBar::handle:horizontal:hover {"
            "    background: #4f46e5;"
            "}"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
            "    width: 0px;"
            "}"
        ));
        m_btnTheme->setText(QStringLiteral("🌙 Dark"));
    } else {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    font-family: \"Noto Sans\", \"Ubuntu\", \"DejaVu Sans\", \"Symbola\", \"Noto Color Emoji\", \"Segoe UI Emoji\", sans-serif;"
            "}"
            "QMainWindow {"
            "    background-color: #f8fafc;"
            "    color: #0f172a;"
            "}"
            "QSplitter::handle {"
            "    background-color: #cbd5e1;"
            "}"
            "QLineEdit, QComboBox {"
            "    background-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 5px;"
            "    color: #0f172a;"
            "    padding: 5px 8px;"
            "}"
            "QLineEdit:focus, QComboBox:focus {"
            "    border-color: #4f46e5;"
            "}"
            "QComboBox QAbstractItemView {"
            "    background-color: #ffffff;"
            "    color: #0f172a;"
            "    selection-background-color: #4f46e5;"
            "    selection-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 5px;"
            "}"
            "QPushButton {"
            "    background-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 5px;"
            "    color: #0f172a;"
            "    padding: 5px 12px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #f1f5f9;"
            "    border-color: #4f46e5;"
            "}"
            "QPushButton:checked {"
            "    background-color: #4f46e5;"
            "    color: #ffffff;"
            "    border-color: #4338ca;"
            "}"
            "QListWidget {"
            "    background-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 6px;"
            "    color: #0f172a;"
            "    outline: none;"
            "    padding: 4px;"
            "}"
            "QListWidget::item {"
            "    padding: 6px 10px;"
            "    border-radius: 5px;"
            "    margin: 1px 2px;"
            "}"
            "QListWidget::item:hover {"
            "    background-color: #f1f5f9;"
            "}"
            "QListWidget::item:selected {"
            "    background-color: #e0f2fe;"
            "    color: #0284c7;"
            "    font-weight: bold;"
            "}"
            "QFrame {"
            "    background-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 6px;"
            "}"
            "QFrame#batchToolbar {"
            "    background-color: #f0f5ff;"
            "    border: 1px solid #b4c8f0;"
            "    border-radius: 8px;"
            "}"
            "QFrame#similarityBanner {"
            "    background-color: #e0e7ff;"
            "    border: 1px solid #6366f1;"
            "    border-radius: 6px;"
            "}"
            "QLabel {"
            "    background-color: transparent;"
            "    color: #0f172a;"
            "}"
            "QScrollBar:vertical {"
            "    background: #f8fafc;"
            "    width: 8px;"
            "    margin: 0px;"
            "}"
            "QScrollBar::handle:vertical {"
            "    background: #cbd5e1;"
            "    min-height: 20px;"
            "    border-radius: 4px;"
            "}"
            "QScrollBar::handle:vertical:hover {"
            "    background: #4f46e5;"
            "}"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
            "    height: 0px;"
            "}"
            "QScrollBar:horizontal {"
            "    background: #f8fafc;"
            "    height: 8px;"
            "    margin: 0px;"
            "}"
            "QScrollBar::handle:horizontal {"
            "    background: #cbd5e1;"
            "    min-width: 20px;"
            "    border-radius: 4px;"
            "}"
            "QScrollBar::handle:horizontal:hover {"
            "    background: #4f46e5;"
            "}"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
            "    width: 0px;"
            "}"
        ));
        m_btnTheme->setText(QStringLiteral("☀️ Light"));
    }

    updateColorPillStyles();
    updateAspectPillStyles();

    if (m_delegate) m_delegate->setDarkMode(m_darkMode);
    if (m_view) m_view->setDarkMode(m_darkMode);
    if (m_viewerWindow) m_viewerWindow->setDarkMode(m_darkMode);
}

void MainWindow::changeRootDir(const QString& dir) {
    const QString absDir = QDir(dir).absolutePath();
    if (absDir == m_currentRootDir) {
        startScan();
        return;
    }
    m_currentRootDir = absDir;
    // Folder scope, favourites-only and similarity belong to the old root.
    m_filter.resetForNewRoot();
    m_thumbMgr.cancelPending();
    // Show what is already indexed for the new root while it is rescanned.
    refreshFolderList();
    refreshGallery();
    startScan();
}

void MainWindow::startScan() {
    m_lblStats->setText(m_dbError.isEmpty() ? QStringLiteral("Scanning...")
                                            : QStringLiteral("⚠ Database unavailable: %1").arg(m_dbError));
    if (m_folderPathEdit) {
        m_folderPathEdit->setText(m_currentRootDir);
    }
    m_watcher->cancelPending();
    m_rescanPending = false;

    if (m_scanner) {
        // Cancel without blocking the GUI thread; the old scanner deletes
        // itself once its thread ends, and its late signals are ignored.
        m_scanner->cancel();
        m_scanner = nullptr;
    }

    auto* scanner = new Scanner(m_currentRootDir, m_db.dbPath(), this);
    m_scanner = scanner;
    m_scanInProgress = true;
    connect(scanner, &Scanner::progress, this, [this, scanner](int count) {
        if (scanner == m_scanner) onScanProgress(count);
    });
    connect(scanner, &Scanner::scanFinished, this,
            [this, scanner](int count, const Stats& stats, const QString& error) {
        onScanFinished(scanner, count, stats, error);
    });
    connect(scanner, &QThread::finished, scanner, &QObject::deleteLater);
    scanner->start();
}

void MainWindow::onScanProgress(int count) {
    m_lblStats->setText(QStringLiteral("Found %1 images...").arg(count));
}

void MainWindow::onScanFinished(Scanner* scanner, int totalCount, const Stats& stats, const QString& error) {
    if (scanner != m_scanner) {
        return; // a superseded (cancelled) scan
    }
    m_scanInProgress = false;

    QString label;
    if (!m_dbError.isEmpty()) {
        label = QStringLiteral("⚠ Database unavailable: %1").arg(m_dbError);
    } else if (!error.isEmpty()) {
        label = QStringLiteral("⚠ %1").arg(error);
    } else {
        const double gb = static_cast<double>(stats.totalBytes) / (1024.0 * 1024.0 * 1024.0);
        label = QStringLiteral("Total: %1 images (%2 GB)")
            .arg(QString::number(totalCount), QString::number(gb, 'f', 1));
    }

    refreshFolderList();
    refreshGallery();

    m_thumbMgr.startBackgroundIndexing(m_currentRootDir);
    m_watcher->setRoot(m_currentRootDir);
    if (!m_watcher->unwatchedDirectories().isEmpty()) {
        label += QStringLiteral(" — live updates limited (%1 folders not watched)")
                     .arg(m_watcher->unwatchedDirectories().size());
    }
    m_lblStats->setText(label);

    if (m_rescanPending) {
        // Changes arrived while scanning; the walk may already have passed them.
        startScan();
    }
}

void MainWindow::onWatcherRescanRequested() {
    if (m_scanInProgress) {
        m_rescanPending = true;
        return;
    }
    startScan();
}

bool MainWindow::refreshFolderList() {
    const QSignalBlocker blocker(m_folderList);
    m_folderList->clear();
    const auto subfolders = m_db.getSubfoldersWithCounts(m_currentRootDir);
    int totalAll = 0;
    bool selectedStillExists = m_filter.scope() == FilterState::Scope::AllImages
                               || m_filter.scope() == FilterState::Scope::Favorites;
    for (const auto& pair : subfolders) {
        totalAll += pair.second;
        const auto scope = pair.first.isEmpty() ? FilterState::Scope::RootFolder : FilterState::Scope::Subfolder;
        selectedStillExists = selectedStillExists || m_filter.isSelected(scope, pair.first);
    }

    auto addItem = [this](const QString& label, FilterState::Scope scope, const QString& sub) {
        auto* item = new QListWidgetItem(label, m_folderList);
        item->setData(Qt::UserRole, static_cast<int>(scope));
        item->setData(Qt::UserRole + 1, sub);
        return item;
    };

    addItem(QStringLiteral("🖼️ All Images (%1)").arg(totalAll), FilterState::Scope::AllImages, QString());
    const Stats stats = m_db.getStats(m_currentRootDir);
    addItem(QStringLiteral("⭐ Favorites (%1)").arg(stats.favoritesCount), FilterState::Scope::Favorites, QString());

    const QString filterText = m_folderSearch->text().trimmed().toLower();
    for (const auto& pair : subfolders) {
        const bool isRoot = pair.first.isEmpty();
        const QString label = isRoot
            ? QStringLiteral("📁 Root Folder (%1)").arg(pair.second)
            : QStringLiteral("📁 %1 (%2)").arg(pair.first, QString::number(pair.second));
        if (!filterText.isEmpty() && !label.toLower().contains(filterText)) {
            continue;
        }
        addItem(label, isRoot ? FilterState::Scope::RootFolder : FilterState::Scope::Subfolder, pair.first);
    }

    bool reset = false;
    if (!selectedStillExists) {
        // The selected folder disappeared (deleted, renamed, new root): show
        // everything rather than an empty gallery under a stale filter.
        m_filter.selectAllImages();
        reset = true;
    }

    m_folderList->setCurrentRow(-1);
    for (int i = 0; i < m_folderList->count(); ++i) {
        auto* it = m_folderList->item(i);
        const auto scope = static_cast<FilterState::Scope>(it->data(Qt::UserRole).toInt());
        if (m_filter.isSelected(scope, it->data(Qt::UserRole + 1).toString())) {
            m_folderList->setCurrentItem(it);
            break;
        }
    }
    return reset;
}

void MainWindow::refreshGallery() {
    QVector<ImageRecord> items = m_db.queryImages(m_filter.toQueryFilter(m_currentRootDir));

    if (m_filter.hasSimilarityTarget()) {
        items = FilterState::rankBySimilarity(items, m_filter.similarityHash());
        if (m_lblSimilarity) {
            m_lblSimilarity->setText(QStringLiteral("✨ Showing %1 visually similar images to '%2'")
                .arg(QString::number(items.size()), m_filter.similarityName()));
        }
        if (m_similarityBanner) {
            m_similarityBanner->setVisible(true);
        }
    } else if (m_similarityBanner) {
        m_similarityBanner->setVisible(false);
    }

    m_model->setItems(std::move(items));
    m_view->viewport()->update();
}

void MainWindow::onFindSimilarRequested(const QString& dhash, const QString& filename) {
    m_filter.setSimilarityTarget(dhash, filename);
    refreshGallery();
}

void MainWindow::onClearSimilarityClicked() {
    m_filter.clearSimilarityTarget();
    refreshGallery();
}

void MainWindow::onThumbnailReady(const QString& sourcePath, const QString& /*thumbPath*/, const ImageFeatures& feat) {
    m_model->updateItemFeatures(sourcePath, feat);
}

void MainWindow::onChangeFolderClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Image Directory"), m_currentRootDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    if (!dir.isEmpty()) {
        changeRootDir(dir);
    }
}

void MainWindow::onRescanClicked() {
    startScan();
}

void MainWindow::onSearchTextChanged(const QString& text) {
    m_filter.setSearchTerm(text);
    m_searchTimer->start();
}

void MainWindow::onColorPillClicked(const QString& colorId) {
    m_filter.toggleColor(colorId);
    updateColorPillStyles();
    refreshGallery();
}

void MainWindow::onAspectPillClicked(const QString& aspectId) {
    m_filter.setAspectType(aspectId);
    updateAspectPillStyles();
    refreshGallery();
}

void MainWindow::onSortChanged(int index) {
    m_filter.setSortBy(m_sortCombo->itemData(index).toString());
    refreshGallery();
}

void MainWindow::onCardSizeChanged(int value) {
    // The delegate announces the new size hint; no reset (keeps the selection).
    m_delegate->setCardSize(value);
}

void MainWindow::setSelectModeButtonStyle(int count) {
    if (m_selectMode) {
        m_btnSelectMode->setText(QStringLiteral("☑ Selecting (%1)").arg(count));
        m_btnSelectMode->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "    background-color: #12263e;"
            "    color: #00d2ff;"
            "    border: 1.5px solid #00d2ff;"
            "    font-weight: bold;"
            "    border-radius: 5px;"
            "    padding: 5px 12px;"
            "}"
        ));
    } else {
        m_btnSelectMode->setText(QStringLiteral("☑ Batch Select"));
        m_btnSelectMode->setStyleSheet(QString());
    }
}

void MainWindow::onSelectModeToggled() {
    m_selectMode = m_btnSelectMode->isChecked();
    m_view->setSelectMode(m_selectMode);
    const int count = static_cast<int>(m_view->getSelectedPaths().size());
    setSelectModeButtonStyle(count);
    m_batchBar->setVisible(m_selectMode || count > 0);
}

void MainWindow::onThemeToggled() {
    m_darkMode = !m_darkMode;
    applyTheme();
}

void MainWindow::onFolderItemClicked(QListWidgetItem* item) {
    if (!item) return;
    const auto scope = static_cast<FilterState::Scope>(item->data(Qt::UserRole).toInt());
    const QString sub = item->data(Qt::UserRole + 1).toString();
    switch (scope) {
    case FilterState::Scope::AllImages: m_filter.selectAllImages(); break;
    case FilterState::Scope::Favorites: m_filter.selectFavorites(); break;
    case FilterState::Scope::RootFolder: m_filter.selectRootFolder(); break;
    case FilterState::Scope::Subfolder: m_filter.selectSubfolder(sub); break;
    }
    refreshGallery();
}

void MainWindow::onFolderSearchChanged(const QString& /*text*/) {
    if (refreshFolderList()) {
        refreshGallery();
    }
}

void MainWindow::onSelectionCountChanged(int count, const QStringList& paths) {
    m_lblBatchCount->setText(QStringLiteral("📦 %1 item(s) selected").arg(count));
    m_lblBatchCount->setStyleSheet(QStringLiteral("color: #38bdf8; font-weight: bold; font-size: 13px;"));
    m_btnBatchDrag->setFilePaths(paths);
    if (count > 0) {
        m_btnBatchDrag->setText(QStringLiteral("📤 Drag to Attach (%1)").arg(count));
        if (m_btnCopyFiles) m_btnCopyFiles->setText(QStringLiteral("📁 Copy Files (%1)").arg(count));
        if (m_btnCopyPaths) m_btnCopyPaths->setText(QStringLiteral("📋 Copy Paths (%1)").arg(count));
    } else {
        m_btnBatchDrag->setText(QStringLiteral("📤 Drag to Attach"));
        if (m_btnCopyFiles) m_btnCopyFiles->setText(QStringLiteral("📁 Copy Files (Attach)"));
        if (m_btnCopyPaths) m_btnCopyPaths->setText(QStringLiteral("📋 Copy Paths"));
    }
    setSelectModeButtonStyle(count);
    m_batchBar->setVisible(m_selectMode || count > 0);
}

void MainWindow::onOpenViewer(const ImageRecord& item, int /*row*/) {
    // Resolve by path: the row may be stale (e.g. chosen from a context menu
    // while a refresh reordered the gallery). The viewer gets the model's
    // up-to-date records (favourite state included).
    const int row = m_model->rowForPath(item.path);
    if (row < 0) return;
    m_viewerWindow->showImage(m_model->items(), row);
}

void MainWindow::onFavoriteToggled(int /*row*/, const QString& path) {
    toggleFavoriteForPath(path);
}

void MainWindow::toggleFavoriteForPath(const QString& path) {
    bool newVal = false;
    if (!m_db.toggleFavorite(path, &newVal)) {
        m_lblStats->setText(QStringLiteral("⚠ Could not update favourite: %1").arg(m_db.lastError()));
        return;
    }
    m_model->setItemFavorite(path, newVal);
    m_viewerWindow->setFavoriteState(path, newVal);
    refreshFolderList();
    if (m_filter.scope() == FilterState::Scope::Favorites) {
        refreshGallery();
    }
}

} // namespace OmniView
