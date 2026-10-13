#pragma once

#include <QMainWindow>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVector>
#include "Database.h"

namespace OmniView {

class DragButton : public QPushButton {
    Q_OBJECT
public:
    explicit DragButton(const QString& text, QWidget* parent = nullptr);
    void setFilePath(const QString& path) {
        m_filePaths = path.isEmpty() ? QStringList() : QStringList{path};
    }
    void setFilePaths(const QStringList& paths) { m_filePaths = paths; }
    QStringList filePaths() const { return m_filePaths; }
    QString filePath() const { return m_filePaths.isEmpty() ? QString() : m_filePaths.first(); }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QStringList m_filePaths;
    QPoint m_dragStartPos;
    bool m_pressed = false; // (0,0) is a valid press position, so track it explicitly
};

class ViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ViewerWindow(QWidget* parent = nullptr);

    void showImage(const QVector<ImageRecord>& items, int index);
    int currentIndex() const { return m_currentIndex; }
    double zoomFactor() const { return m_zoomFactor; }
    bool fitMode() const { return m_fitMode; }

    static constexpr double MIN_ZOOM = 0.01;
    static constexpr double MAX_ZOOM = 10.0;
    static constexpr int MAX_VIEW_DIMENSION = 8192;
    void setDarkMode(bool dark);
    void setFavoriteState(const QString& path, bool favorite);

signals:
    void favoriteToggled(int index, const QString& path);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void zoomIn();
    void zoomOut();
    void zoomFit();
    void zoomOriginal();
    void showPrevious();
    void showNext();
    void copyFile();
    void copyPath();
    void copyImage();
    void openDefaultViewer();
    void toggleFavorite();
    void showInFolder();

private:
    void loadCurrentImage();
    void updateDisplay();
    void applyTheme();
    void zoomBy(double factor);

    QVector<ImageRecord> m_items;
    int m_currentIndex = 0;
    double m_zoomFactor = 1.0;
    bool m_fitMode = true;
    bool m_darkMode = true;

    QScrollArea* m_scrollArea = nullptr;
    QLabel* m_imageLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    QPushButton* m_btnFav = nullptr;
    DragButton* m_dragBtn = nullptr;

    QImage m_currentImage;
    QSize m_originalSize;      // full, orientation-corrected size of the source
    QString m_loadError;
    QPoint m_panStartPos;
    bool m_panning = false;
    int m_wheelAccumulator = 0;
};

} // namespace OmniView
