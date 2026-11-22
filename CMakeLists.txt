cmake_minimum_required(VERSION 3.25)

project(glimply VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets)
find_package(KF6Config REQUIRED)
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6StatusNotifierItem REQUIRED)

add_executable(glimply
    src/main.cpp
)

target_link_libraries(glimply
    PRIVATE
        Qt6::Widgets
        KF6::ConfigCore
        KF6::GlobalAccel
        KF6::StatusNotifierItem
)

find_package(Qt6 REQUIRED COMPONENTS DBus Gui)
add_library(glimply_capture STATIC
    src/captureservice.cpp
    src/imageoutput.cpp
)
target_include_directories(glimply_capture PUBLIC src)
target_link_libraries(glimply_capture PUBLIC Qt6::Gui PRIVATE Qt6::DBus)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(imageoutputtest tests/imageoutputtest.cpp)
    target_link_libraries(imageoutputtest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME imageoutput COMMAND imageoutputtest)

    # Opt-in only: this executable reads the real desktop and is never run by CTest.
    add_executable(glimply-capture-smoke EXCLUDE_FROM_ALL tests/capturesmoke.cpp)
    target_link_libraries(glimply-capture-smoke PRIVATE glimply_capture)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/org.glimply.CaptureSmoke.desktop"
        CONTENT "[Desktop Entry]\nType=Application\nName=glimply capture smoke test\nNoDisplay=true\nExec=\"$<TARGET_FILE:glimply-capture-smoke>\"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n")
endif()
