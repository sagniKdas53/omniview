#pragma once

#include <QThread>
#include <QString>
#include <atomic>
#include "Database.h"

namespace OmniView {

class Scanner : public QThread {
    Q_OBJECT
public:
    explicit Scanner(QString rootDir, QString dbPath = QString(), QObject* parent = nullptr);
    ~Scanner() override;

    Q_DISABLE_COPY_MOVE(Scanner)

    void cancel();
    bool isCancelled() const { return m_cancelled.load(); }
    QString rootDir() const { return m_rootDir; }

signals:
    void progress(int count);
    // Emitted once when a scan runs to completion (never after cancel()).
    // error is empty on success; on a database error the scan stops early and
    // nothing is pruned.
    void scanFinished(int totalCount, const OmniView::Stats& stats, const QString& error);

protected:
    void run() override;

private:
    QString m_rootDir;
    QString m_dbPath;
    std::atomic<bool> m_cancelled{false};
};

} // namespace OmniView
