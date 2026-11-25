cmake_minimum_required(VERSION 3.25)

project(glimply VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Concurrent DBus Gui LinguistTools)
find_package(KF6Config 6.13 REQUIRED)
find_package(KF6GlobalAccel 6.13 REQUIRED)
find_package(KF6StatusNotifierItem 6.13 REQUIRED)
find_package(KF6Notifications 6.13 REQUIRED)
find_package(KF6DBusAddons 6.13 REQUIRED)
find_package(KF6WidgetsAddons 6.13 REQUIRED)
find_package(KF6XmlGui 6.13 REQUIRED)
find_package(KF6I18n 6.13 REQUIRED)

add_executable(glimply
    src/main.cpp
    src/app.cpp
    src/colorpicker.cpp
    src/controlservice.cpp
    src/captureoverlay.cpp
    src/pinmanager.cpp
    src/printkey.cpp
    src/settingswindow.cpp
    src/shortcutmanager.cpp
    src/traycontroller.cpp
    src/trayicon.cpp
)
target_compile_definitions(glimply PRIVATE GLIMPLY_VERSION="${PROJECT_VERSION}")
# Brand marks for the footer of the settings window.
qt_add_resources(glimply "brand" PREFIX "/brand" BASE data FILES data/brand-roteson.png data/brand-r2w2.png)

target_link_libraries(glimply
    PRIVATE
        glimply_capture
        glimply_ui
        Qt6::Concurrent
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
    src/annotations/annotationcommands.cpp
    src/annotations/annotationdocument.cpp
    src/annotations/annotationgeometry.cpp
    src/annotations/annotationitem.cpp
    src/annotations/annotationrenderer.cpp
    src/annotations/annotationsettings.cpp
    src/annotations/annotationtextlayout.cpp
    src/annotations/overlayexport.cpp
    src/annotations/programmaticannotator.cpp
    src/annotations/redactionprocessor.cpp
    src/captureservice.cpp
    src/cli.cpp
    src/colorformat.cpp
    src/historystore.cpp
    src/i18n.cpp
    src/hookrunner.cpp
    src/imageoutput.cpp
    src/intervalcontroller.cpp
    src/kwinpinscript.cpp
    src/latencytrace.cpp
    src/mcppolicy.cpp
    src/mcpserver.cpp
    src/ocrengine.cpp
    src/pinstore.cpp
    src/screens.cpp
    src/selectiongeometry.cpp
    src/selectionmodel.cpp
    src/targets.cpp
    src/thumbnailcache.cpp
    src/triggerengine.cpp
    src/triggerplan.cpp
    src/windowlist.cpp
)
target_include_directories(glimply_capture PUBLIC src)
target_compile_definitions(glimply_capture PUBLIC GLIMPLY_VERSION="${PROJECT_VERSION}")
target_link_libraries(glimply_capture PUBLIC Qt6::Gui KF6::ConfigCore PRIVATE Qt6::Concurrent Qt6::DBus)

add_library(glimply_ui STATIC
    src/annotations/annotationcanvas.cpp
    src/annotations/annotationcontroller.cpp
    src/annotations/annotationtoolbar.cpp
    src/annotations/inlinetexteditor.cpp
    src/areaselector.cpp
    src/countdowntimer.cpp
    src/exportdialog.cpp
    src/mcpconfirmdialog.cpp
    src/overlaypanel.cpp
    src/pinwindow.cpp
    src/pixelloupe.cpp
    src/quickeditorwindow.cpp
)
target_include_directories(glimply_ui PUBLIC src)
target_link_libraries(glimply_ui PUBLIC glimply_capture Qt6::Widgets KF6::WidgetsAddons KF6::I18n PRIVATE Qt6::Concurrent)

# Translations (#51). The application uses Qt catalogs (i18n/glimply_<lang>.ts, compiled to .qm);
# the Plasma widget uses gettext catalogs (plasmoid/po). English is the source language;
# i18n/glimply_en.ts only carries the English plural forms. Refresh the sources with
# scripts/update-translations.sh.
set(QT_I18N_SOURCE_LANGUAGE en)
set(QT_I18N_TRANSLATED_LANGUAGES ar de es fr hi id it ja ko nl pl pt ru tr uk vi zh_CN zh_TW)
qt_add_translations(glimply
    SOURCE_TARGETS glimply glimply_ui glimply_capture
    TS_FILE_BASE glimply
    TS_FILE_DIR i18n
    QM_FILES_OUTPUT_VARIABLE glimply_qm_files
    LUPDATE_OPTIONS -no-obsolete -locations none
)
install(FILES ${glimply_qm_files} DESTINATION ${CMAKE_INSTALL_DATADIR}/glimply/translations)
ki18n_install(plasmoid/po)

# KWin grants access to org.kde.KWin.ScreenShot2 to the executable named in this desktop file, so
# Exec must be the absolute installed path.
set(GLIMPLY_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/glimply")
configure_file(data/io.github.m00sfett.glimply.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.glimply.desktop" @ONLY)
install(TARGETS glimply RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.glimply.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/glimply.notifyrc DESTINATION ${CMAKE_INSTALL_DATADIR}/knotifications6)
install(FILES data/io.github.m00sfett.glimply.svg data/io.github.m00sfett.glimply-tray.svg
              data/io.github.m00sfett.glimply-tray-flash.svg
              data/io.github.m00sfett.glimply-tray-interval.svg
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
    add_executable(selectiongeometrytest tests/selectiongeometrytest.cpp)
    target_link_libraries(selectiongeometrytest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME selectiongeometry COMMAND selectiongeometrytest)
    add_executable(selectionmodeltest tests/selectionmodeltest.cpp)
    target_link_libraries(selectionmodeltest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME selectionmodel COMMAND selectionmodeltest)
    add_executable(annotationenginetest tests/annotationenginetest.cpp)
    target_link_libraries(annotationenginetest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME annotationengine COMMAND annotationenginetest)
    add_executable(coremarkuptoolstest tests/coremarkuptoolstest.cpp)
    target_link_libraries(coremarkuptoolstest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME coremarkuptools COMMAND coremarkuptoolstest)
    add_executable(textannotationtest tests/textannotationtest.cpp)
    target_link_libraries(textannotationtest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME textannotation COMMAND textannotationtest)
    add_executable(redactiontest tests/redactiontest.cpp)
    target_link_libraries(redactiontest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME redaction COMMAND redactiontest)
    add_executable(overlaymarkuptest tests/overlaymarkuptest.cpp)
    target_link_libraries(overlaymarkuptest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME overlaymarkup COMMAND overlaymarkuptest)
    add_executable(quickeditortest tests/quickeditortest.cpp)
    target_link_libraries(quickeditortest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME quickeditor COMMAND quickeditortest)
    add_executable(exporttest tests/exporttest.cpp)
    target_link_libraries(exporttest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME export COMMAND exporttest)
    add_executable(delaytest tests/delaytest.cpp)
    target_link_libraries(delaytest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME delay COMMAND delaytest)
    add_executable(crossmonitortest tests/crossmonitortest.cpp)
    target_link_libraries(crossmonitortest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME crossmonitor COMMAND crossmonitortest)
    add_executable(loupepickertest tests/loupepickertest.cpp)
    target_link_libraries(loupepickertest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME loupepicker COMMAND loupepickertest)
    add_executable(intervaltest tests/intervaltest.cpp)
    target_link_libraries(intervaltest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME interval COMMAND intervaltest)
    add_executable(overlaypaneltest tests/overlaypaneltest.cpp)
    target_link_libraries(overlaypaneltest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME overlaypanel COMMAND overlaypaneltest)
    add_executable(latencytracetest tests/latencytracetest.cpp)
    target_link_libraries(latencytracetest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME latencytrace COMMAND latencytracetest)
    add_executable(clitest tests/clitest.cpp)
    target_link_libraries(clitest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME cli COMMAND clitest)
    add_executable(hooktest tests/hooktest.cpp)
    target_link_libraries(hooktest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME hook COMMAND hooktest)
    add_executable(mcptest tests/mcptest.cpp)
    target_link_libraries(mcptest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME mcp COMMAND mcptest)
    add_executable(historytest tests/historytest.cpp)
    target_link_libraries(historytest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME history COMMAND historytest)
    add_executable(pintest tests/pintest.cpp)
    target_link_libraries(pintest PRIVATE glimply_ui Qt6::Test)
    add_test(NAME pin COMMAND pintest)
    add_executable(i18ntest tests/i18ntest.cpp)
    target_link_libraries(i18ntest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME i18n COMMAND i18ntest)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME translations
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-translations.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_executable(ocrtest tests/ocrtest.cpp)
    target_link_libraries(ocrtest PRIVATE glimply_capture Qt6::Test)
    add_test(NAME ocr COMMAND ocrtest)

    # Opt-in visual check of the overlay panel (#69); renders PNG files, never run by CTest.
    add_executable(glimply-panel-shots EXCLUDE_FROM_ALL tests/panelshots.cpp)
    target_link_libraries(glimply-panel-shots PRIVATE glimply_ui)

    # Opt-in only: this executable reads the real desktop and is never run by CTest.
    add_executable(glimply-capture-smoke EXCLUDE_FROM_ALL tests/capturesmoke.cpp)
    target_link_libraries(glimply-capture-smoke PRIVATE glimply_capture)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/org.glimply.CaptureSmoke.desktop"
        CONTENT "[Desktop Entry]\nType=Application\nName=glimply capture smoke test\nNoDisplay=true\nExec=\"$<TARGET_FILE:glimply-capture-smoke>\"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n")
endif()
