#pragma once

#include <QString>
#include <QVector>

#include "Database.h"

namespace OmniView {

// Everything that decides which images the gallery shows, kept as one value
// with explicit transitions instead of loose members on MainWindow.
class FilterState {
public:
    enum class Scope {
        AllImages,
        Favorites,
        RootFolder,
        Subfolder
    };

    // Maximum dHash Hamming distance for "visually similar".
    static constexpr int SIMILARITY_THRESHOLD = 18;

    Scope scope() const { return m_scope; }
    QString subfolder() const { return m_subfolder; }
    bool isSelected(Scope scope, const QString& subfolder = QString()) const;

    void selectAllImages();
    void selectFavorites();
    void selectRootFolder();
    // An empty name selects the root folder.
    void selectSubfolder(const QString& subfolder);

    QString colorName() const { return m_colorName; }
    // Choosing the active colour again (or "all") clears the colour filter.
    void toggleColor(const QString& colorId);
    QString aspectType() const { return m_aspectType; }
    void setAspectType(const QString& aspectId);
    QString searchTerm() const { return m_searchTerm; }
    void setSearchTerm(const QString& term) { m_searchTerm = term; }
    QString sortBy() const { return m_sortBy; }
    void setSortBy(const QString& sortBy) { m_sortBy = sortBy.isEmpty() ? QStringLiteral("mtime_desc") : sortBy; }

    bool hasSimilarityTarget() const { return !m_similarityHash.isEmpty(); }
    QString similarityHash() const { return m_similarityHash; }
    QString similarityName() const { return m_similarityName; }
    void setSimilarityTarget(const QString& dhash, const QString& filename);
    void clearSimilarityTarget();

    // A new root folder: folder scope, favourites and similarity no longer
    // apply; colour, aspect, sort and search are user preferences and stay.
    void resetForNewRoot();

    QueryFilter toQueryFilter(const QString& rootDir) const;

    // Keeps records within SIMILARITY_THRESHOLD of dhash, closest first; ties
    // keep their incoming (user-chosen sort) order.
    static QVector<ImageRecord> rankBySimilarity(const QVector<ImageRecord>& records,
                                                 const QString& dhash,
                                                 int threshold = SIMILARITY_THRESHOLD);

private:
    Scope m_scope = Scope::AllImages;
    QString m_subfolder;
    QString m_colorName = QStringLiteral("all");
    QString m_aspectType = QStringLiteral("all");
    QString m_searchTerm;
    QString m_sortBy = QStringLiteral("mtime_desc");
    QString m_similarityHash;
    QString m_similarityName;
};

} // namespace OmniView
