#pragma once

#include <QThread>
#include <QString>
#include <atomic>
#include "Database.h"

namespace OmniView {

class Scanner : public QThread {
    Q_OBJECT
public:
    explicit Scanner(const QString& rootDir, const QString& dbPath = QString(), QObject* parent = nullptr);
    ~Scanner() override;

    void cancel();

signals:
    void progress(int count);
    void finished(int totalCount, const Stats& stats);

protected:
    void run() override;

private:
    QString m_rootDir;
    QString m_dbPath;
    std::atomic<bool> m_cancelled{false};
};

} // namespace OmniView
