cmake_minimum_required(VERSION 3.25)

project(glimply VERSION 0.1.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets)
find_package(KF6Config REQUIRED)
find_package(KF6GlobalAccel REQUIRED)
find_package(KF6StatusNotifierItem REQUIRED)
find_package(KF6Notifications REQUIRED)
find_package(KF6DBusAddons REQUIRED)
find_package(KF6WidgetsAddons REQUIRED)
find_package(KF6XmlGui REQUIRED)
find_package(KF6I18n REQUIRED)

add_executable(glimply
    src/main.cpp
    src/app.cpp
    src/controlservice.cpp
    src/areaselector.cpp
    src/settingswindow.cpp
    src/screens.cpp
    src/shortcutmanager.cpp
    src/traycontroller.cpp
    src/trayicon.cpp
)
target_compile_definitions(glimply PRIVATE GLIMPLY_VERSION="${PROJECT_VERSION}")

target_link_libraries(glimply
    PRIVATE
        glimply_capture
        Qt6::DBus
        Qt6::Widgets
        KF6::ConfigCore
        KF6::DBusAddons
        KF6::GlobalAccel
        KF6::I18n
        KF6::Notifications
        KF6::StatusNotifierItem
        KF6::WidgetsAddons
        KF6::XmlGui
)

find_package(Qt6 REQUIRED COMPONENTS Concurrent DBus Gui)
add_library(glimply_capture STATIC
    src/captureservice.cpp
    src/imageoutput.cpp
    src/targets.cpp
    src/triggerengine.cpp
    src/triggerplan.cpp
)
target_include_directories(glimply_capture PUBLIC src)
target_link_libraries(glimply_capture PUBLIC Qt6::Gui KF6::ConfigCore PRIVATE Qt6::Concurrent Qt6::DBus)

# KWin grants access to org.kde.KWin.ScreenShot2 to the executable named in this desktop file, so
# Exec must be the absolute installed path.
set(GLIMPLY_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/glimply")
configure_file(data/io.github.m00sfett.glimply.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.glimply.desktop" @ONLY)
install(TARGETS glimply RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.glimply.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/glimply.notifyrc DESTINATION ${CMAKE_INSTALL_DATADIR}/knotifications6)
install(FILES data/io.github.m00sfett.glimply.svg data/io.github.m00sfett.glimply-symbolic.svg
              data/io.github.m00sfett.glimply-flash-symbolic.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# D-Bus activation: the Plasma widget starts glimply on demand.
configure_file(data/io.github.m00sfett.glimply.service.in "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.glimply.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.glimply.service" DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

# Plasma widget (QML package).
set(GLIMPLY_VERSION "${PROJECT_VERSION}")
set(GLIMPLY_PLASMOID_DIR ${CMAKE_INSTALL_DATADIR}/plasma/plasmoids/io.github.m00sfett.glimply.widget)
configure_file(plasmoid/metadata.json "${CMAKE_CURRENT_BINARY_DIR}/plasmoid/metadata.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plasmoid/metadata.json" DESTINATION ${GLIMPLY_PLASMOID_DIR})
install(DIRECTORY plasmoid/contents DESTINATION ${GLIMPLY_PLASMOID_DIR})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(imageoutputtest tests/imageoutputtest.cpp)
    target_link_libraries(imageoutputtest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME imageoutput COMMAND imageoutputtest)
    add_executable(targetstest tests/targetstest.cpp)
    target_link_libraries(targetstest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME targets COMMAND targetstest)

    # Opt-in only: this executable reads the real desktop and is never run by CTest.
    add_executable(glimply-capture-smoke EXCLUDE_FROM_ALL tests/capturesmoke.cpp)
    target_link_libraries(glimply-capture-smoke PRIVATE glimply_capture)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/org.glimply.CaptureSmoke.desktop"
        CONTENT "[Desktop Entry]\nType=Application\nName=glimply capture smoke test\nNoDisplay=true\nExec=\"$<TARGET_FILE:glimply-capture-smoke>\"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n")
endif()
