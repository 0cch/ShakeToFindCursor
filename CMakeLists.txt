cmake_minimum_required(VERSION 3.15)

project(ShakeToFindCursor
        VERSION 1.4.0
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STFC_BUILD_TESTS "Build the detector unit tests (run with ctest)" ON)
option(STFC_STRICT_WARNINGS "Treat warnings as errors when building" ON)
option(STFC_REQUIRE_ADMIN
       "Embed a requireAdministrator manifest instead of asInvoker")

enable_testing()

# ---------------------------------------------------------------------------
# Shake detector tests. Header-only, no Windows dependency, so they build and
# run on any platform - which is how the detection algorithm gets regression
# coverage without a Windows machine.
# ---------------------------------------------------------------------------
if(STFC_BUILD_TESTS)
    add_executable(shake_detector_tests tests/shake_detector_test.cpp)
    target_include_directories(shake_detector_tests PRIVATE src)
    if(MSVC)
        target_compile_options(shake_detector_tests PRIVATE /W3 /EHsc /utf-8)
    else()
        target_compile_options(shake_detector_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME shake_detector_tests COMMAND shake_detector_tests)
endif()

if(NOT WIN32)
    message(STATUS "ShakeToFindCursor: not on Windows - only the detector "
                   "tests were configured.")
    return()
endif()

add_definitions(-DUNICODE -D_UNICODE)

set(STFC_SOURCES
    main.cpp
    src/app_settings.cpp
    src/auto_start.cpp
    src/find_overlay.cpp
    src/pointer_zoomer.cpp
    src/win_util.cpp)

# ---------------------------------------------------------------------------
# Resources. res.rc is generated with absolute paths for the icon and the
# manifest: rc.exe and windres disagree about how a relative resource file name
# is resolved, and the manifest is what carries the DPI awareness plus the
# asInvoker / requireAdministrator choice, so it must be embedded exactly once.
# ---------------------------------------------------------------------------
if(STFC_REQUIRE_ADMIN)
    set(STFC_MANIFEST_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/app.requireAdministrator.manifest")
else()
    set(STFC_MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()
# rc.exe runs quoted resource strings through its own escape processing, so a
# backslash path such as D:\a\ShakeToFindCursor\app_icon.ico can come out
# mangled (\a) or rejected outright. Forward slashes are accepted by both
# rc.exe and windres, which is also what CMake itself writes into the .rc files
# it generates for the Visual Studio generators.
string(REPLACE "\\" "/" STFC_MANIFEST_FILE "${STFC_MANIFEST_FILE}")
string(REPLACE "\\" "/" STFC_ICON_FILE
       "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.ico")
set(STFC_MANIFEST_LINE
    "1 24 \"${STFC_MANIFEST_FILE}\"")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/res.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/res.rc" @ONLY)

add_executable(${PROJECT_NAME}
    WIN32
    ${STFC_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/res.rc")

target_include_directories(${PROJECT_NAME} PRIVATE src)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /MP
        /EHsc
        /utf-8)
    if(STFC_STRICT_WARNINGS)
        target_compile_options(${PROJECT_NAME} PRIVATE /WX)
    endif()
    # The manifest is embedded by the resource compiler; keep the linker from
    # generating (and then merging) a second one.
    target_link_options(${PROJECT_NAME} PRIVATE "/MANIFEST:NO")
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -municode)
    if(STFC_STRICT_WARNINGS)
        target_compile_options(${PROJECT_NAME} PRIVATE -Werror)
    endif()
    # The Task Scheduler COM API, plus the COM runtime the AutoStart code
    # initializes on demand. MSVC gets these from the #pragma comment lines.
    target_link_libraries(${PROJECT_NAME} PRIVATE
        taskschd ole32 oleaut32 shell32)
    # Picks the startup object that calls wWinMain instead of WinMain.
    target_link_options(${PROJECT_NAME} PRIVATE "-municode")
endif()

message(STATUS "ShakeToFindCursor ${PROJECT_VERSION}: manifest "
               "${STFC_MANIFEST_FILE}")
