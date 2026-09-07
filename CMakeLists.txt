cmake_minimum_required(VERSION 3.16)
project(OmniView VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Sql Concurrent Test)

# Common sources
set(OMNIVIEW_LIB_SOURCES
    src/miniz.c
    src/ZipUtils.cpp
    src/Config.cpp
    src/Database.cpp
    src/ColorUtils.cpp
    src/Scanner.cpp
    src/ThumbnailWorker.cpp
    src/GalleryModel.cpp
    src/GalleryDelegate.cpp
    src/GalleryView.cpp
    src/ViewerWindow.cpp
    src/MainWindow.cpp
)

set(OMNIVIEW_HEADERS
    src/miniz.h
    src/ZipUtils.h
    src/Config.h
    src/Database.h
    src/ColorUtils.h
    src/Scanner.h
    src/ThumbnailWorker.h
    src/GalleryModel.h
    src/GalleryDelegate.h
    src/GalleryView.h
    src/ViewerWindow.h
    src/MainWindow.h
)

add_executable(omniview-cpp
    src/main.cpp
    ${OMNIVIEW_LIB_SOURCES}
    ${OMNIVIEW_HEADERS}
)

target_include_directories(omniview-cpp PRIVATE src)
target_link_libraries(omniview-cpp PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    Qt5::Concurrent
)

# Test target
enable_testing()
add_executable(omniview_tests
    tests/test_omniview.cpp
    ${OMNIVIEW_LIB_SOURCES}
    ${OMNIVIEW_HEADERS}
)
target_include_directories(omniview_tests PRIVATE src)
target_link_libraries(omniview_tests PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    Qt5::Concurrent
    Qt5::Test
)

add_test(NAME OmniViewTests COMMAND omniview_tests)
