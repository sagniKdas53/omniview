#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QComboBox>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QFrame>

#include "Database.h"
#include "Scanner.h"
#include "ThumbnailWorker.h"
#include "GalleryModel.h"
#include "GalleryDelegate.h"
#include "GalleryView.h"
#include "ViewerWindow.h"

namespace OmniView {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString& initialDir = QString(), QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onChangeFolderClicked();
    void onRescanClicked();
    void onSearchTextChanged(const QString& text);
    void onColorChanged(int index);
    void onAspectChanged(int index);
    void onSortChanged(int index);
    void onCardSizeChanged(int value);
    void onSelectModeToggled();
    void onThemeToggled();

    void onFolderItemClicked(QListWidgetItem* item);
    void onFolderSearchChanged(const QString& text);

    void onScanProgress(int count);
    void onScanFinished(int totalCount, const Stats& stats);

    void onFsDirectoryChanged(const QString& path);
    void onFsTimeout();

    void onThumbnailReady(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& feat);
    void onSelectionCountChanged(int count, const QStringList& paths);
    void onOpenViewer(const ImageRecord& item, int row);
    void onFavoriteToggled(int row, const QString& path);

private:
    void setupUi();
    QWidget* createTopBar();
    QWidget* createSidebar();
    QWidget* createBatchToolbar();
    void applyTheme();

    void startScan();
    void refreshGallery();
    void refreshFolderList();
    void updateFsWatchers();

    QString m_currentRootDir;
    Database m_db;
    ThumbnailManager m_thumbMgr;
    Scanner* m_scanner = nullptr;

    GalleryModel* m_model = nullptr;
    GalleryDelegate* m_delegate = nullptr;
    GalleryView* m_view = nullptr;
    ViewerWindow* m_viewerWindow = nullptr;

    // Filter state
    QString m_selectedSubfolder = QStringLiteral("__all__");
    QString m_selectedColor = QStringLiteral("all");
    QString m_selectedAspect = QStringLiteral("all");
    QString m_searchTerm;
    bool m_favoriteOnly = false;
    QString m_sortBy = QStringLiteral("mtime_desc");
    bool m_darkMode = true;
    bool m_selectMode = false;
    QVector<ImageRecord> m_currentItems;

    // UI elements
    QPushButton* m_btnChangeFolder = nullptr;
    QPushButton* m_btnRescan = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_colorCombo = nullptr;
    QComboBox* m_aspectCombo = nullptr;
    QComboBox* m_sortCombo = nullptr;
    QSlider* m_cardSizeSlider = nullptr;
    QPushButton* m_btnSelectMode = nullptr;
    QPushButton* m_btnTheme = nullptr;
    QLabel* m_lblStats = nullptr;

    QLineEdit* m_folderSearch = nullptr;
    QListWidget* m_folderList = nullptr;

    // Batch Bar
    QFrame* m_batchBar = nullptr;
    QLabel* m_lblBatchCount = nullptr;
    DragButton* m_btnBatchDrag = nullptr;
    QPushButton* m_btnCopyFiles = nullptr;
    QPushButton* m_btnCopyPaths = nullptr;

    // Watcher & Debounce
    QFileSystemWatcher* m_fsWatcher = nullptr;
    QTimer* m_fsTimer = nullptr;
    QTimer* m_searchTimer = nullptr;
};

} // namespace OmniView
