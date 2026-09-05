#include "MainWindow.h"
#include "Config.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QApplication>

namespace OmniView {

MainWindow::MainWindow(const QString& initialDir, QWidget* parent)
    : QMainWindow(parent)
    , m_currentRootDir(initialDir.isEmpty() ? Config::defaultRootDir() : initialDir)
    , m_thumbMgr(m_db.dbPath())
{
    Config::ensureDirectories();

    setWindowTitle(Config::appTitle());
    resize(1380, 880);

    m_viewerWindow = new ViewerWindow(this);
    connect(m_viewerWindow, &ViewerWindow::favoriteToggled, this, [this](int /*idx*/, const QString& path) {
        bool newVal = false;
        if (m_db.toggleFavorite(path, &newVal)) {
            const int r = m_model->rowForPath(path);
            if (r >= 0) {
                m_model->setItemFavorite(r, newVal);
            }
        }
    });

    // File System Watcher for live updates
    m_fsWatcher = new QFileSystemWatcher(this);
    connect(m_fsWatcher, &QFileSystemWatcher::directoryChanged, this, &MainWindow::onFsDirectoryChanged);
    m_fsTimer = new QTimer(this);
    m_fsTimer->setSingleShot(true);
    m_fsTimer->setInterval(1200);
    connect(m_fsTimer, &QTimer::timeout, this, &MainWindow::onFsTimeout);

    // Search debounce timer
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(300);
    connect(m_searchTimer, &QTimer::timeout, this, &MainWindow::refreshGallery);

    // Thumbnail generation signals
    connect(&m_thumbMgr, &ThumbnailManager::thumbnailReady, this, &MainWindow::onThumbnailReady);

    setupUi();
    applyTheme();
    startScan();
}

MainWindow::~MainWindow() {
    if (m_scanner && m_scanner->isRunning()) {
        m_scanner->cancel();
        m_scanner->wait();
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
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);

    m_btnChangeFolder = new QPushButton(QStringLiteral("📁 %1").arg(QFileInfo(m_currentRootDir).fileName()), this);
    m_btnChangeFolder->setToolTip(QStringLiteral("Change root directory"));
    connect(m_btnChangeFolder, &QPushButton::clicked, this, &MainWindow::onChangeFolderClicked);
    layout->addWidget(m_btnChangeFolder);

    m_btnRescan = new QPushButton(QStringLiteral("🔄 Rescan"), this);
    m_btnRescan->setToolTip(QStringLiteral("Rescan files in current directory"));
    connect(m_btnRescan, &QPushButton::clicked, this, &MainWindow::onRescanClicked);
    layout->addWidget(m_btnRescan);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("🔍 Search files and subfolders..."));
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    layout->addWidget(m_searchEdit, 1);

    // Color filter
    m_colorCombo = new QComboBox(this);
    for (const auto& item : ColorUtils::palette()) {
        m_colorCombo->addItem(item.label, item.id);
    }
    connect(m_colorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onColorChanged);
    layout->addWidget(m_colorCombo);

    // Aspect filter
    m_aspectCombo = new QComboBox(this);
    m_aspectCombo->addItem(QStringLiteral("All Shapes"), QStringLiteral("all"));
    m_aspectCombo->addItem(QStringLiteral("Landscape ⛶"), QStringLiteral("landscape"));
    m_aspectCombo->addItem(QStringLiteral("Portrait ▯"), QStringLiteral("portrait"));
    m_aspectCombo->addItem(QStringLiteral("Square ◻"), QStringLiteral("square"));
    connect(m_aspectCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onAspectChanged);
    layout->addWidget(m_aspectCombo);

    // Sort
    m_sortCombo = new QComboBox(this);
    m_sortCombo->addItem(QStringLiteral("📅 Newest First"), QStringLiteral("mtime_desc"));
    m_sortCombo->addItem(QStringLiteral("📅 Oldest First"), QStringLiteral("mtime_asc"));
    m_sortCombo->addItem(QStringLiteral("📦 Largest Size"), QStringLiteral("size_desc"));
    m_sortCombo->addItem(QStringLiteral("📦 Smallest Size"), QStringLiteral("size_asc"));
    m_sortCombo->addItem(QStringLiteral("🔤 Filename A-Z"), QStringLiteral("name_asc"));
    m_sortCombo->addItem(QStringLiteral("🎲 Random"), QStringLiteral("random"));
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onSortChanged);
    layout->addWidget(m_sortCombo);

    // Card Size slider
    auto* lblSize = new QLabel(QStringLiteral("Size:"), this);
    layout->addWidget(lblSize);
    m_cardSizeSlider = new QSlider(Qt::Horizontal, this);
    m_cardSizeSlider->setRange(Config::MIN_GRID_SIZE, Config::MAX_GRID_SIZE);
    m_cardSizeSlider->setValue(Config::DEFAULT_GRID_SIZE);
    m_cardSizeSlider->setFixedWidth(100);
    connect(m_cardSizeSlider, &QSlider::valueChanged, this, &MainWindow::onCardSizeChanged);
    layout->addWidget(m_cardSizeSlider);

    // Select Mode button
    m_btnSelectMode = new QPushButton(QStringLiteral("✓ Select"), this);
    m_btnSelectMode->setCheckable(true);
    connect(m_btnSelectMode, &QPushButton::clicked, this, &MainWindow::onSelectModeToggled);
    layout->addWidget(m_btnSelectMode);

    // Theme Toggle button
    m_btnTheme = new QPushButton(QStringLiteral("🌙"), this);
    m_btnTheme->setFixedWidth(36);
    m_btnTheme->setToolTip(QStringLiteral("Toggle Dark / Light mode"));
    connect(m_btnTheme, &QPushButton::clicked, this, &MainWindow::onThemeToggled);
    layout->addWidget(m_btnTheme);

    // Stats
    m_lblStats = new QLabel(QStringLiteral("Scanning..."), this);
    layout->addWidget(m_lblStats);

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
    frame->setFrameShape(QFrame::StyledPanel);

    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(10);

    m_lblBatchCount = new QLabel(QStringLiteral("0 items selected"), frame);
    QFont f = m_lblBatchCount->font();
    f.setBold(true);
    m_lblBatchCount->setFont(f);
    layout->addWidget(m_lblBatchCount);

    auto* btnSelectAll = new QPushButton(QStringLiteral("Select All (Ctrl+A)"), frame);
    connect(btnSelectAll, &QPushButton::clicked, m_view, &GalleryView::selectAllItems);
    layout->addWidget(btnSelectAll);

    auto* btnCopyFiles = new QPushButton(QStringLiteral("📋 Copy Files"), frame);
    connect(btnCopyFiles, &QPushButton::clicked, m_view, &GalleryView::copySelectedFiles);
    layout->addWidget(btnCopyFiles);

    auto* btnCopyPaths = new QPushButton(QStringLiteral("🔗 Copy Paths"), frame);
    connect(btnCopyPaths, &QPushButton::clicked, m_view, &GalleryView::copySelectedPaths);
    layout->addWidget(btnCopyPaths);

    // Direct Batch Drag Button!
    m_btnBatchDrag = new DragButton(QStringLiteral("🖐 Drag Selected to Attach"), frame);
    m_btnBatchDrag->setStyleSheet(QStringLiteral(
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
    layout->addWidget(m_btnBatchDrag);

    layout->addStretch(1);

    auto* btnClear = new QPushButton(QStringLiteral("✕ Clear (Esc)"), frame);
    connect(btnClear, &QPushButton::clicked, m_view, &GalleryView::clearAllSelection);
    layout->addWidget(btnClear);

    return frame;
}

void MainWindow::applyTheme() {
    if (m_darkMode) {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    background-color: #121520;"
            "    color: #e2e8f0;"
            "}"
            "QSplitter::handle {"
            "    background-color: #262a38;"
            "}"
            "QLineEdit, QComboBox, QListWidget {"
            "    background-color: #1a1e2d;"
            "    border: 1px solid #2d3748;"
            "    border-radius: 5px;"
            "    color: #e2e8f0;"
            "    padding: 5px 8px;"
            "}"
            "QLineEdit:focus, QComboBox:focus {"
            "    border-color: #6366f1;"
            "}"
            "QPushButton {"
            "    background-color: #24293c;"
            "    border: 1px solid #3b4259;"
            "    border-radius: 5px;"
            "    color: #e2e8f0;"
            "    padding: 5px 12px;"
            "}"
            "QPushButton:hover {"
            "    background-color: #2f364e;"
            "}"
            "QPushButton:checked {"
            "    background-color: #4f46e5;"
            "    color: #ffffff;"
            "    border-color: #6366f1;"
            "}"
            "QListWidget::item:selected {"
            "    background-color: #4f46e5;"
            "    color: #ffffff;"
            "    border-radius: 4px;"
            "}"
            "QFrame {"
            "    background-color: #1e2436;"
            "    border: 1px solid #3b4259;"
            "    border-radius: 6px;"
            "}"
        ));
        m_btnTheme->setText(QStringLiteral("🌙"));
    } else {
        setStyleSheet(QStringLiteral(
            "QMainWindow, QWidget {"
            "    background-color: #f8fafc;"
            "    color: #0f172a;"
            "}"
            "QSplitter::handle {"
            "    background-color: #cbd5e1;"
            "}"
            "QLineEdit, QComboBox, QListWidget {"
            "    background-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 5px;"
            "    color: #0f172a;"
            "    padding: 5px 8px;"
            "}"
            "QLineEdit:focus, QComboBox:focus {"
            "    border-color: #4f46e5;"
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
            "}"
            "QPushButton:checked {"
            "    background-color: #4f46e5;"
            "    color: #ffffff;"
            "    border-color: #4338ca;"
            "}"
            "QListWidget::item:selected {"
            "    background-color: #4f46e5;"
            "    color: #ffffff;"
            "    border-radius: 4px;"
            "}"
            "QFrame {"
            "    background-color: #ffffff;"
            "    border: 1px solid #cbd5e1;"
            "    border-radius: 6px;"
            "}"
        ));
        m_btnTheme->setText(QStringLiteral("☀️"));
    }

    if (m_delegate) m_delegate->setDarkMode(m_darkMode);
    if (m_view) m_view->setDarkMode(m_darkMode);
    if (m_viewerWindow) m_viewerWindow->setDarkMode(m_darkMode);
}

void MainWindow::startScan() {
    m_lblStats->setText(QStringLiteral("Scanning %1...").arg(m_currentRootDir));
    m_btnChangeFolder->setText(QStringLiteral("📁 %1").arg(QFileInfo(m_currentRootDir).fileName()));

    if (m_scanner && m_scanner->isRunning()) {
        m_scanner->cancel();
        m_scanner->wait();
        delete m_scanner;
        m_scanner = nullptr;
    }

    m_scanner = new Scanner(m_currentRootDir, m_db.dbPath(), this);
    connect(m_scanner, &Scanner::progress, this, &MainWindow::onScanProgress);
    connect(m_scanner, &Scanner::finished, this, &MainWindow::onScanFinished);
    m_scanner->start();
}

void MainWindow::onScanProgress(int count) {
    m_lblStats->setText(QStringLiteral("Found %1 images so far...").arg(count));
}

void MainWindow::onScanFinished(int totalCount, const Stats& stats) {
    const double gb = static_cast<double>(stats.totalBytes) / (1024.0 * 1024.0 * 1024.0);
    m_lblStats->setText(QStringLiteral("%1 images (%2 GB)")
        .arg(totalCount)
        .arg(gb, 0, 'f', 1)
    );

    refreshFolderList();
    refreshGallery();

    m_thumbMgr.startBackgroundIndexing();
    updateFsWatchers();
}

void MainWindow::updateFsWatchers() {
    const QStringList watched = m_fsWatcher->directories();
    if (!watched.isEmpty()) {
        m_fsWatcher->removePaths(watched);
    }

    if (QDir(m_currentRootDir).exists()) {
        QStringList paths = {m_currentRootDir};
        QDir rootQDir(m_currentRootDir);
        const auto subdirs = rootQDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& fi : subdirs) {
            if (!fi.fileName().startsWith('.')) {
                paths.append(fi.absoluteFilePath());
                if (paths.size() >= 200) break;
            }
        }
        m_fsWatcher->addPaths(paths);
    }
}

void MainWindow::onFsDirectoryChanged(const QString& /*path*/) {
    m_fsTimer->start();
}

void MainWindow::onFsTimeout() {
    if (!m_scanner || !m_scanner->isRunning()) {
        startScan();
    }
}

void MainWindow::refreshFolderList() {
    m_folderList->clear();
    const auto subfolders = m_db.getSubfoldersWithCounts(m_currentRootDir);
    int totalAll = 0;
    for (const auto& pair : subfolders) {
        totalAll += pair.second;
    }

    // 1. All images
    auto* itemAll = new QListWidgetItem(QStringLiteral("🖼️ All Images (%1)").arg(totalAll), m_folderList);
    itemAll->setData(Qt::UserRole, QStringLiteral("__all__"));

    // 2. Favorites
    const Stats stats = m_db.getStats();
    auto* itemFav = new QListWidgetItem(QStringLiteral("⭐ Favorites (%1)").arg(stats.favoritesCount), m_folderList);
    itemFav->setData(Qt::UserRole, QStringLiteral("__fav__"));

    // 3. Subfolders
    const QString filterText = m_folderSearch->text().trimmed().toLower();
    for (const auto& pair : subfolders) {
        QString label;
        if (pair.first == QStringLiteral("__root__")) {
            label = QStringLiteral("📁 Root Folder (%1)").arg(pair.second);
        } else {
            label = QStringLiteral("📁 %1 (%2)").arg(pair.first).arg(pair.second);
        }

        if (!filterText.isEmpty() && !label.toLower().contains(filterText)) {
            continue;
        }

        auto* item = new QListWidgetItem(label, m_folderList);
        item->setData(Qt::UserRole, pair.first);
    }

    // Preserve selection
    for (int i = 0; i < m_folderList->count(); ++i) {
        auto* it = m_folderList->item(i);
        if (it->data(Qt::UserRole).toString() == m_selectedSubfolder) {
            m_folderList->setCurrentItem(it);
            return;
        }
    }
    m_folderList->setCurrentRow(0);
}

void MainWindow::refreshGallery() {
    QueryFilter filter;
    filter.rootDir = m_currentRootDir;
    filter.subfolder = m_selectedSubfolder;
    filter.colorName = m_selectedColor;
    filter.aspectType = m_selectedAspect;
    filter.searchTerm = m_searchTerm;
    filter.favoriteOnly = m_favoriteOnly;
    filter.sortBy = m_sortBy;

    m_currentItems = m_db.queryImages(filter);
    m_model->setItems(m_currentItems);

    m_view->viewport()->update();
}

void MainWindow::onThumbnailReady(const QString& sourcePath, const QString& /*thumbPath*/, const ImageFeatures& feat) {
    m_model->updateItemFeatures(sourcePath, feat);
}

void MainWindow::onChangeFolderClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Image Directory"), m_currentRootDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    if (!dir.isEmpty() && dir != m_currentRootDir) {
        m_currentRootDir = dir;
        m_selectedSubfolder = QStringLiteral("__all__");
        startScan();
    }
}

void MainWindow::onRescanClicked() {
    startScan();
}

void MainWindow::onSearchTextChanged(const QString& text) {
    m_searchTerm = text;
    m_searchTimer->start();
}

void MainWindow::onColorChanged(int index) {
    m_selectedColor = m_colorCombo->itemData(index).toString();
    refreshGallery();
}

void MainWindow::onAspectChanged(int index) {
    m_selectedAspect = m_aspectCombo->itemData(index).toString();
    refreshGallery();
}

void MainWindow::onSortChanged(int index) {
    m_sortBy = m_sortCombo->itemData(index).toString();
    refreshGallery();
}

void MainWindow::onCardSizeChanged(int value) {
    m_delegate->setCardSize(value);
    m_view->setSpacing(8);
    m_view->reset(); // Forces relayout with new size hints
}

void MainWindow::onSelectModeToggled() {
    m_selectMode = m_btnSelectMode->isChecked();
    m_view->setSelectMode(m_selectMode);
    if (!m_selectMode) {
        m_view->clearAllSelection();
        m_batchBar->hide();
    }
}

void MainWindow::onThemeToggled() {
    m_darkMode = !m_darkMode;
    applyTheme();
}

void MainWindow::onFolderItemClicked(QListWidgetItem* item) {
    if (!item) return;
    const QString sub = item->data(Qt::UserRole).toString();
    m_selectedSubfolder = sub;
    m_favoriteOnly = (sub == QStringLiteral("__fav__"));
    refreshGallery();
}

void MainWindow::onFolderSearchChanged(const QString& /*text*/) {
    refreshFolderList();
}

void MainWindow::onSelectionCountChanged(int count, const QStringList& paths) {
    if (count > 0 && m_selectMode) {
        m_lblBatchCount->setText(QStringLiteral("%1 items selected").arg(count));
        m_batchBar->show();
        if (!paths.isEmpty()) {
            m_btnBatchDrag->setFilePath(paths.first());
        }
    } else {
        m_batchBar->hide();
    }
}

void MainWindow::onOpenViewer(const ImageRecord& /*item*/, int row) {
    m_viewerWindow->showImage(m_currentItems, row);
}

void MainWindow::onFavoriteToggled(int row, const QString& path) {
    bool newVal = false;
    if (m_db.toggleFavorite(path, &newVal)) {
        m_model->setItemFavorite(row, newVal);
        // If viewing favorites, refresh list
        if (m_favoriteOnly) {
            refreshGallery();
        }
    }
}

} // namespace OmniView
