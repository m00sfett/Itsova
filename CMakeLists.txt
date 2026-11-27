cmake_minimum_required(VERSION 3.25)

project(itsova VERSION 0.12.0 LANGUAGES C CXX)

set(ITSOVA_PRERELEASE "" CACHE STRING "Optional prerelease label without leading hyphen (e.g. beta.1)")
if(NOT ITSOVA_PRERELEASE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _itsova_exact_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_itsova_exact_tag MATCHES "^v${PROJECT_VERSION}-([0-9A-Za-z.-]+)$")
            set(ITSOVA_PRERELEASE "${CMAKE_MATCH_1}")
        endif()
    endif()
endif()
if(ITSOVA_PRERELEASE)
    set(ITSOVA_VERSION "${PROJECT_VERSION}-${ITSOVA_PRERELEASE}")
else()
    set(ITSOVA_VERSION "${PROJECT_VERSION}")
endif()

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
# Screen recording (#30): KWin's screencast protocol and KPipeWire's encoder.
find_package(Qt6 6.8 REQUIRED COMPONENTS WaylandClient)
find_package(PlasmaWaylandProtocols 1.10 REQUIRED)
find_package(KPipeWire 6.3 REQUIRED)
# Recording hints (#125) are layer-shell surfaces, placed exactly beside what is recorded.
find_package(LayerShellQt 6.3 REQUIRED)

add_executable(itsova
    src/main.cpp
    src/app.cpp
    src/app_capture.cpp
    src/app_mcp.cpp
    src/app_recording.cpp
    src/colorpicker.cpp
    src/controlservice.cpp
    src/captureoverlay.cpp
    src/captureoverlay_panel.cpp
    src/capturesession.cpp
    src/pinmanager.cpp
    src/printkey.cpp
    src/settingswindow.cpp
    src/shortcutmanager.cpp
    src/traycontroller.cpp
)
target_compile_definitions(itsova PRIVATE ITSOVA_VERSION="${ITSOVA_VERSION}")
# Brand marks for the footer of the settings window.
qt_add_resources(itsova "brand" PREFIX "/brand" BASE data FILES data/brand-roteson.png data/brand-r2w2.png)

target_link_libraries(itsova
    PRIVATE
        itsova_capture
        itsova_record
        itsova_ui
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
        K::KPipeWireRecord
        Qt6::WaylandClient
)

find_package(Qt6 REQUIRED COMPONENTS Concurrent DBus Gui)
add_library(itsova_capture STATIC
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
    src/logging.cpp
    src/mcppolicy.cpp
    src/mcpserver.cpp
    src/ocrengine.cpp
    src/pinstore.cpp
    src/recordingfinalizer.cpp
    src/recordingmetadata.cpp
    src/recordingplan.cpp
    src/screens.cpp
    src/selectiongeometry.cpp
    src/selectionmodel.cpp
    src/targets.cpp
    src/thumbnailcache.cpp
    src/triggerengine.cpp
    src/triggerplan.cpp
    src/windowlist.cpp
    src/windowpick.cpp
)
target_include_directories(itsova_capture PUBLIC src)
target_compile_definitions(itsova_capture PUBLIC ITSOVA_VERSION="${ITSOVA_VERSION}")
target_link_libraries(itsova_capture PUBLIC Qt6::Gui KF6::ConfigCore PRIVATE Qt6::Concurrent Qt6::DBus)

# The recorder lives in its own library so tests can drive it without the application (#92, #90).
add_library(itsova_record STATIC src/screenrecorder.cpp)
target_include_directories(itsova_record PUBLIC src)
target_link_libraries(itsova_record PUBLIC itsova_capture Qt6::Gui PRIVATE K::KPipeWireRecord Qt6::WaylandClient)
# Older KPipeWire (Debian 13: 6.3) has no pause and no object serials; older protocol files no
# serial event. Recording works there, pausing does not.
get_target_property(itsova_kpipewire_includes K::KPipeWire INTERFACE_INCLUDE_DIRECTORIES)
find_file(ITSOVA_KPIPEWIRE_STREAM_HEADER pipewirebaseencodedstream.h PATHS ${itsova_kpipewire_includes}
          PATH_SUFFIXES KPipeWire NO_DEFAULT_PATH)
if(ITSOVA_KPIPEWIRE_STREAM_HEADER)
    file(READ "${ITSOVA_KPIPEWIRE_STREAM_HEADER}" itsova_kpipewire_stream)
    if(itsova_kpipewire_stream MATCHES "void pause\\(\\)")
        target_compile_definitions(itsova_record PUBLIC ITSOVA_KPIPEWIRE_PAUSE=1)
    endif()
    if(itsova_kpipewire_stream MATCHES "setObjectSerial")
        target_compile_definitions(itsova_record PUBLIC ITSOVA_KPIPEWIRE_SERIAL=1)
    endif()
endif()
file(READ "${PLASMA_WAYLAND_PROTOCOLS_DIR}/zkde-screencast-unstable-v1.xml" itsova_screencast_xml)
if(itsova_screencast_xml MATCHES "name=\"serial\"")
    target_compile_definitions(itsova_record PUBLIC ITSOVA_SCREENCAST_SERIAL_EVENT=1)
endif()
qt6_generate_wayland_protocol_client_sources(itsova_record PRIVATE_CODE
    FILES "${PLASMA_WAYLAND_PROTOCOLS_DIR}/zkde-screencast-unstable-v1.xml")

add_library(itsova_ui STATIC
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
    src/recordinghint.cpp
    src/trayicon.cpp
)
target_include_directories(itsova_ui PUBLIC src)
target_link_libraries(itsova_ui PUBLIC itsova_capture Qt6::Widgets KF6::WidgetsAddons KF6::I18n PRIVATE Qt6::Concurrent LayerShellQt::Interface)

# Translations (#51). The application uses Qt catalogs (i18n/itsova_<lang>.ts, compiled to .qm);
# the Plasma widget uses gettext catalogs (plasmoid/po). English is the source language;
# i18n/itsova_en.ts only carries the English plural forms. Refresh the sources with
# scripts/update-translations.sh.
set(QT_I18N_SOURCE_LANGUAGE en)
set(QT_I18N_TRANSLATED_LANGUAGES ar de es fr hi id it ja ko nl pl pt ru tr uk vi zh_CN zh_TW)
qt_add_translations(itsova
    SOURCE_TARGETS itsova itsova_ui itsova_capture itsova_record
    TS_FILE_BASE itsova
    TS_FILE_DIR i18n
    QM_FILES_OUTPUT_VARIABLE itsova_qm_files
    LUPDATE_OPTIONS -no-obsolete -locations none
)
install(FILES ${itsova_qm_files} DESTINATION ${CMAKE_INSTALL_DATADIR}/itsova/translations)
ki18n_install(plasmoid/po)

# KWin grants access to org.kde.KWin.ScreenShot2 to the executable named in this desktop file, so
# Exec must be the absolute installed path.
set(ITSOVA_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/itsova")
configure_file(data/io.github.m00sfett.itsova.desktop.in "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.itsova.desktop" @ONLY)
install(TARGETS itsova RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.itsova.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.m00sfett.itsova.notifyrc DESTINATION ${CMAKE_INSTALL_DATADIR}/knotifications6)
install(FILES data/io.github.m00sfett.itsova.svg data/io.github.m00sfett.itsova-tray.svg
              data/io.github.m00sfett.itsova-tray-flash.svg
              data/io.github.m00sfett.itsova-tray-interval.svg
              data/io.github.m00sfett.itsova-tray-recording.svg
              data/io.github.m00sfett.itsova-tray-recording-paused.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# D-Bus activation: the Plasma widget starts itsova on demand.
configure_file(data/io.github.m00sfett.itsova.service.in "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.itsova.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.m00sfett.itsova.service" DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

# Plasma widget (QML package).
set(ITSOVA_PLASMOID_DIR ${CMAKE_INSTALL_DATADIR}/plasma/plasmoids/io.github.m00sfett.itsova.widget)
configure_file(plasmoid/metadata.json "${CMAKE_CURRENT_BINARY_DIR}/plasmoid/metadata.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plasmoid/metadata.json" DESTINATION ${ITSOVA_PLASMOID_DIR})
install(DIRECTORY plasmoid/contents DESTINATION ${ITSOVA_PLASMOID_DIR})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(imageoutputtest tests/imageoutputtest.cpp)
    target_link_libraries(imageoutputtest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME imageoutput COMMAND imageoutputtest)
    add_executable(targetstest tests/targetstest.cpp)
    target_link_libraries(targetstest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME targets COMMAND targetstest)
    add_executable(selectiongeometrytest tests/selectiongeometrytest.cpp)
    target_link_libraries(selectiongeometrytest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME selectiongeometry COMMAND selectiongeometrytest)
    add_executable(selectionmodeltest tests/selectionmodeltest.cpp)
    target_link_libraries(selectionmodeltest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME selectionmodel COMMAND selectionmodeltest)
    add_executable(annotationenginetest tests/annotationenginetest.cpp)
    target_link_libraries(annotationenginetest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME annotationengine COMMAND annotationenginetest)
    add_executable(coremarkuptoolstest tests/coremarkuptoolstest.cpp)
    target_link_libraries(coremarkuptoolstest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME coremarkuptools COMMAND coremarkuptoolstest)
    add_executable(textannotationtest tests/textannotationtest.cpp)
    target_link_libraries(textannotationtest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME textannotation COMMAND textannotationtest)
    add_executable(redactiontest tests/redactiontest.cpp)
    target_link_libraries(redactiontest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME redaction COMMAND redactiontest)
    add_executable(overlaymarkuptest tests/overlaymarkuptest.cpp)
    target_link_libraries(overlaymarkuptest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME overlaymarkup COMMAND overlaymarkuptest)
    add_executable(quickeditortest tests/quickeditortest.cpp)
    target_link_libraries(quickeditortest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME quickeditor COMMAND quickeditortest)
    add_executable(exporttest tests/exporttest.cpp)
    target_link_libraries(exporttest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME export COMMAND exporttest)
    add_executable(delaytest tests/delaytest.cpp)
    target_link_libraries(delaytest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME delay COMMAND delaytest)
    add_executable(crossmonitortest tests/crossmonitortest.cpp)
    target_link_libraries(crossmonitortest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME crossmonitor COMMAND crossmonitortest)
    add_executable(loupepickertest tests/loupepickertest.cpp)
    target_link_libraries(loupepickertest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME loupepicker COMMAND loupepickertest)
    add_executable(intervaltest tests/intervaltest.cpp)
    target_link_libraries(intervaltest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME interval COMMAND intervaltest)
    add_executable(overlaypaneltest tests/overlaypaneltest.cpp)
    target_link_libraries(overlaypaneltest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME overlaypanel COMMAND overlaypaneltest)
    add_executable(latencytracetest tests/latencytracetest.cpp)
    target_link_libraries(latencytracetest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME latencytrace COMMAND latencytracetest)
    add_executable(clitest tests/clitest.cpp)
    target_link_libraries(clitest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME cli COMMAND clitest)
    add_executable(hooktest tests/hooktest.cpp)
    target_link_libraries(hooktest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME hook COMMAND hooktest)
    add_executable(mcptest tests/mcptest.cpp)
    target_link_libraries(mcptest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME mcp COMMAND mcptest)
    add_executable(historytest tests/historytest.cpp)
    target_link_libraries(historytest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME history COMMAND historytest)
    add_executable(pintest tests/pintest.cpp)
    target_link_libraries(pintest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME pin COMMAND pintest)
    add_executable(i18ntest tests/i18ntest.cpp)
    target_link_libraries(i18ntest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME i18n COMMAND i18ntest)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME profile-migration
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-profile-migration.py")
        add_test(NAME translations
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-translations.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_executable(ocrtest tests/ocrtest.cpp)
    target_link_libraries(ocrtest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME ocr COMMAND ocrtest)
    add_executable(trayicontest tests/trayicontest.cpp)
    target_link_libraries(trayicontest PRIVATE itsova_ui Qt6::Test)
    add_test(NAME trayicon COMMAND trayicontest)
    add_executable(windowpicktest tests/windowpicktest.cpp)
    target_link_libraries(windowpicktest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME windowpick COMMAND windowpicktest)
    add_executable(loggingtest tests/loggingtest.cpp)
    target_link_libraries(loggingtest PRIVATE itsova_record Qt6::Test)
    target_compile_definitions(loggingtest PRIVATE ITSOVA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME logging COMMAND loggingtest)
    add_executable(recordingtest tests/recordingtest.cpp)
    target_link_libraries(recordingtest PRIVATE itsova_capture Qt6::Test)
    add_test(NAME recording COMMAND recordingtest)

    # Widget tests never show windows on the desktop and never depend on a compositor's timing
    # (#76): CTest runs every test on the offscreen platform.
    get_property(itsova_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${itsova_tests} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Nested-KWin acceptance tests (#93): a virtual KWin with a throw-away HOME per test, never the
    # desktop session. Label "nested"; skipped (exit 77) where a tool is missing. install.sh leaves
    # them out (ctest -LE nested).
    find_program(ITSOVA_KWIN_WAYLAND kwin_wayland)
    find_program(ITSOVA_WAYLAND_SCANNER wayland-scanner)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ITSOVA_WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    endif()
    if(ITSOVA_KWIN_WAYLAND AND ITSOVA_WAYLAND_SCANNER AND ITSOVA_WAYLAND_CLIENT_FOUND)
        set(itsova_fake_xml "${PLASMA_WAYLAND_PROTOCOLS_DIR}/fake-input.xml")
        set(itsova_fake_dir "${CMAKE_CURRENT_BINARY_DIR}/fakekey")
        file(MAKE_DIRECTORY "${itsova_fake_dir}")
        add_custom_command(OUTPUT "${itsova_fake_dir}/fake-input.h" "${itsova_fake_dir}/fake-input.c"
            COMMAND ${ITSOVA_WAYLAND_SCANNER} client-header "${itsova_fake_xml}" "${itsova_fake_dir}/fake-input.h"
            COMMAND ${ITSOVA_WAYLAND_SCANNER} private-code "${itsova_fake_xml}" "${itsova_fake_dir}/fake-input.c"
            DEPENDS "${itsova_fake_xml}")
        add_executable(itsova-fakekey tests/nested/fakekey.c "${itsova_fake_dir}/fake-input.c")
        target_include_directories(itsova-fakekey PRIVATE "${itsova_fake_dir}")
        target_link_libraries(itsova-fakekey PRIVATE PkgConfig::ITSOVA_WAYLAND_CLIENT)
        set(itsova_nested "${CMAKE_CURRENT_SOURCE_DIR}/tests/nested")
        function(itsova_nested_test name)
            cmake_parse_arguments(ARG "" "" "OPTIONS" ${ARGN})
            add_test(NAME nested-${name}
                COMMAND "${itsova_nested}/session.sh" ${ARG_OPTIONS} --out "${CMAKE_CURRENT_BINARY_DIR}/nested/${name}"
                        $<TARGET_FILE:itsova> $<TARGET_FILE:itsova-fakekey> "${itsova_nested}/test-${name}.sh")
            set_tests_properties(nested-${name} PROPERTIES LABELS nested RUN_SERIAL TRUE TIMEOUT 240
                                 SKIP_RETURN_CODE 77)
        endfunction()
        itsova_nested_test(overlay)
        itsova_nested_test(pill OPTIONS --monitors 2 --size 1600x900)
        itsova_nested_test(keyboard OPTIONS --monitors 2 --size 1600x900 --apps kcalc)
        itsova_nested_test(pick OPTIONS --apps kcalc)
        itsova_nested_test(loupe OPTIONS --scheme BreezeLight)
        itsova_nested_test(nudge OPTIONS --apps kcalc --scheme BreezeLight)
        itsova_nested_test(recording-hints)
        itsova_nested_test(record-metadata OPTIONS --apps kcalc)
        itsova_nested_test(record-covered-window OPTIONS --apps kcalc)
        itsova_nested_test(record-window-closed OPTIONS --apps kcalc)
        itsova_nested_test(record-window-move OPTIONS --apps kcalc)
        itsova_nested_test(tray-visibility OPTIONS --apps kcalc --rc "[General]\\nTrayIcon=recording")
        itsova_nested_test(record-default OPTIONS --apps kcalc)
    endif()

    # Opt-in check that translated panel button titles fit (#73); never run by CTest.
    add_executable(itsova-title-fit EXCLUDE_FROM_ALL tests/titlefit.cpp)
    target_link_libraries(itsova-title-fit PRIVATE itsova_ui)
    # Opt-in visual check of the overlay panel (#69); renders PNG files, never run by CTest.
    add_executable(itsova-panel-shots EXCLUDE_FROM_ALL tests/panelshots.cpp)
    target_link_libraries(itsova-panel-shots PRIVATE itsova_ui)

    # Opt-in only: this executable reads the real desktop and is never run by CTest.
    add_executable(itsova-capture-smoke EXCLUDE_FROM_ALL tests/capturesmoke.cpp)
    target_link_libraries(itsova-capture-smoke PRIVATE itsova_capture)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/org.itsova.CaptureSmoke.desktop"
        CONTENT "[Desktop Entry]\nType=Application\nName=itsova capture smoke test\nNoDisplay=true\nExec=\"$<TARGET_FILE:itsova-capture-smoke>\"\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n")
endif()
