cmake_minimum_required(VERSION 3.16)
project(OmniView VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Sql Concurrent Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Sql Concurrent Test)

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
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Concurrent
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
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Concurrent
    Qt${QT_VERSION_MAJOR}::Test
)

add_test(NAME OmniViewTests COMMAND omniview_tests)
