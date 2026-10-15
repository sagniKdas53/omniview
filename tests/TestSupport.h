#pragma once

#include <QApplication>
#include <QByteArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "ColorUtils.h"
#include "Database.h"

namespace OmniViewTest {

inline void registerMetaTypes() {
    qRegisterMetaType<OmniView::Stats>("OmniView::Stats");
    qRegisterMetaType<OmniView::Stats>("Stats");
    qRegisterMetaType<OmniView::ImageFeatures>("OmniView::ImageFeatures");
    qRegisterMetaType<OmniView::ImageFeatures>("ImageFeatures");
    qRegisterMetaType<OmniView::ImageRecord>("OmniView::ImageRecord");
    qRegisterMetaType<OmniView::ImageRecord>("ImageRecord");
}

} // namespace OmniViewTest

// Every test binary gets a private, throw-away OmniView cache so tests never
// touch (or depend on) the developer's real ~/.cache/omniview.
#define OMNIVIEW_TEST_MAIN(TestClass)                                   \
    int main(int argc, char** argv) {                                   \
        QTemporaryDir cache;                                            \
        if (!cache.isValid()) return 1;                                 \
        qputenv("OMNIVIEW_CACHE_DIR", cache.path().toUtf8());           \
        QApplication app(argc, argv);                                   \
        QStandardPaths::setTestModeEnabled(true);                       \
        OmniViewTest::registerMetaTypes();                              \
        TestClass test;                                                 \
        return QTest::qExec(&test, argc, argv);                         \
    }
