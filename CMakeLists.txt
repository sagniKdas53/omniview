cmake_minimum_required(VERSION 3.16)
project(OmniView VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(OMNIVIEW_WARNINGS "Enable extra compiler warnings on OmniView's own sources" OFF)
option(OMNIVIEW_WARNINGS_AS_ERRORS "Treat warnings on OmniView's own sources as errors" OFF)
set(OMNIVIEW_SANITIZE "" CACHE STRING
    "Sanitizers to build with: empty, 'address;undefined' or 'thread' (GCC/Clang only)")

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Sql Concurrent Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Sql Concurrent Test)

# --- Sanitizers --------------------------------------------------------------
# Applied to every target (including vendored miniz) so instrumentation is
# consistent; only *warnings* are restricted to project sources.
if(OMNIVIEW_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "OMNIVIEW_SANITIZE is only supported with GCC/Clang")
    endif()
    if("thread" IN_LIST OMNIVIEW_SANITIZE AND "address" IN_LIST OMNIVIEW_SANITIZE)
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
    endif()
    list(JOIN OMNIVIEW_SANITIZE "," _omniview_san)
    add_compile_options(-fsanitize=${_omniview_san} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_omniview_san})
    if("undefined" IN_LIST OMNIVIEW_SANITIZE)
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()
endif()

# --- Vendored miniz: built separately so project warnings never apply -------
add_library(omniview_miniz STATIC src/miniz.c src/miniz.h)
target_include_directories(omniview_miniz PUBLIC src)
set_target_properties(omniview_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Project warnings ---------------------------------------------------------
function(omniview_enable_warnings target)
    if(NOT OMNIVIEW_WARNINGS)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(OMNIVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(OMNIVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# --- Core library shared by the app and the tests ----------------------------
add_library(omniview_core STATIC
    src/ZipUtils.cpp        src/ZipUtils.h
    src/Config.cpp          src/Config.h
    src/Database.cpp        src/Database.h
    src/ColorUtils.cpp      src/ColorUtils.h
    src/Scanner.cpp         src/Scanner.h
    src/ThumbnailWorker.cpp src/ThumbnailWorker.h
    src/GalleryModel.cpp    src/GalleryModel.h
    src/GalleryDelegate.cpp src/GalleryDelegate.h
    src/GalleryView.cpp     src/GalleryView.h
    src/ViewerWindow.cpp    src/ViewerWindow.h
    src/MainWindow.cpp      src/MainWindow.h
)
target_include_directories(omniview_core PUBLIC src)
target_compile_definitions(omniview_core PUBLIC OMNIVIEW_VERSION="${PROJECT_VERSION}")
target_link_libraries(omniview_core PUBLIC
    omniview_miniz
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Sql
    Qt${QT_VERSION_MAJOR}::Concurrent
)
omniview_enable_warnings(omniview_core)

add_executable(omniview-cpp src/main.cpp)
target_link_libraries(omniview-cpp PRIVATE omniview_core)
omniview_enable_warnings(omniview-cpp)

include(GNUInstallDirs)
install(TARGETS omniview-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- Tests ----------------------------------------------------------------------
enable_testing()

set(OMNIVIEW_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tools/sanitizers/lsan.supp:print_suppressions=0"
    "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tools/sanitizers/tsan.supp:halt_on_error=1:second_deadlock_stack=1"
    "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
)

function(omniview_add_test name source)
    add_executable(${name} ${source} tests/TestSupport.h)
    target_link_libraries(${name} PRIVATE omniview_core Qt${QT_VERSION_MAJOR}::Test)
    omniview_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${OMNIVIEW_TEST_ENV}" TIMEOUT 300)
endfunction()

# Historic name kept for the Windows CI artefact step.
add_executable(omniview_tests tests/test_omniview.cpp tests/TestSupport.h)
target_link_libraries(omniview_tests PRIVATE omniview_core Qt${QT_VERSION_MAJOR}::Test)
omniview_enable_warnings(omniview_tests)
add_test(NAME OmniViewTests COMMAND omniview_tests -o -,txt -o omniview-test-results.txt,txt)
set_tests_properties(OmniViewTests PROPERTIES ENVIRONMENT "${OMNIVIEW_TEST_ENV}" TIMEOUT 300)

omniview_add_test(database_regression_tests tests/test_database_regressions.cpp)
omniview_add_test(imaging_tests             tests/test_imaging.cpp)
