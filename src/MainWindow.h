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
#include <QFrame>
#include <QPointer>

#include "Database.h"
#include "FilterState.h"
#include "LibraryWatcher.h"
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

    Q_DISABLE_COPY_MOVE(MainWindow)

    // For tests and scripting.
    QString currentRootDir() const { return m_currentRootDir; }
    const FilterState& filterState() const { return m_filter; }
    bool isScanInProgress() const { return m_scanInProgress; }
    void changeRootDir(const QString& dir);

private slots:
    void onChangeFolderClicked();
    void onRescanClicked();
    void onSearchTextChanged(const QString& text);
    void onColorPillClicked(const QString& colorId);
    void onAspectPillClicked(const QString& aspectId);
    void onSortChanged(int index);
    void onCardSizeChanged(int value);
    void onSelectModeToggled();
    void onThemeToggled();

    void onFolderItemClicked(QListWidgetItem* item);
    void onFolderSearchChanged(const QString& text);

    void onScanProgress(int count);
    void onWatcherRescanRequested();

    void onThumbnailReady(const QString& sourcePath, const QString& thumbPath, const ImageFeatures& feat);
    void onSelectionCountChanged(int count, const QStringList& paths);
    void onOpenViewer(const ImageRecord& item, int row);
    void onFavoriteToggled(int row, const QString& path);
    void toggleFavoriteForPath(const QString& path);
    void onFindSimilarRequested(const QString& dhash, const QString& filename);
    void onClearSimilarityClicked();

private:
    void setupUi();
    QWidget* createTopBar();
    QWidget* createSidebar();
    QWidget* createBatchToolbar();
    void applyTheme();
    void updateColorPillStyles();
    void updateAspectPillStyles();

    void startScan();
    void onScanFinished(Scanner* scanner, int totalCount, const Stats& stats, const QString& error);
    void refreshGallery();
    // Returns true if the selected folder vanished and the filter was reset.
    bool refreshFolderList();
    void setSelectModeButtonStyle(int count);

    QString m_currentRootDir;
    Database m_db;
    QString m_dbError;
    ThumbnailManager m_thumbMgr;
    QPointer<Scanner> m_scanner;
    bool m_scanInProgress = false;
    bool m_rescanPending = false;
    FilterState m_filter;

    GalleryModel* m_model = nullptr;
    GalleryDelegate* m_delegate = nullptr;
    GalleryView* m_view = nullptr;
    ViewerWindow* m_viewerWindow = nullptr;

    bool m_darkMode = true;
    bool m_selectMode = false;

    // UI elements
    QFrame* m_similarityBanner = nullptr;
    QLabel* m_lblSimilarity = nullptr;
    QPushButton* m_btnClearSimilarity = nullptr;

    QLineEdit* m_folderPathEdit = nullptr;
    QPushButton* m_btnBrowse = nullptr;
    QPushButton* m_btnRescan = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QComboBox* m_sortCombo = nullptr;
    QSlider* m_cardSizeSlider = nullptr;
    QPushButton* m_btnSelectMode = nullptr;
    QPushButton* m_btnTheme = nullptr;
    QLabel* m_lblStats = nullptr;

    QVector<QPushButton*> m_colorButtons;
    QVector<QPushButton*> m_aspectButtons;

    QLineEdit* m_folderSearch = nullptr;
    QListWidget* m_folderList = nullptr;

    // Batch Bar
    QFrame* m_batchBar = nullptr;
    QLabel* m_lblBatchCount = nullptr;
    DragButton* m_btnBatchDrag = nullptr;
    QPushButton* m_btnCopyFiles = nullptr;
    QPushButton* m_btnCopyPaths = nullptr;

    // Watcher & Debounce
    LibraryWatcher* m_watcher = nullptr;
    QTimer* m_searchTimer = nullptr;
};

} // namespace OmniView
