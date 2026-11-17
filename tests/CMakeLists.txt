# deepin-MeshLink — self-tests
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>

# Cross/sandbox builds sometimes need the toolchain's own dynamic loader to
# run the test binaries (mixing system ld.so with a toolchain libc aborts).
# Empty by default for native builds.
set(MESHLINK_TEST_INTERP "" CACHE FILEPATH
    "Optional dynamic loader prefix used to run the test executables")

# --- pure-logic unit test (protocol framing, chunk math, notify policy,
#     multi-account storage; offscreen because account creation paints avatars)
add_executable(tst_logic tst_logic.cpp)
target_link_libraries(tst_logic PRIVATE meshlink_core)
add_test(NAME logic COMMAND ${MESHLINK_TEST_INTERP} $<TARGET_FILE:tst_logic>)
set_tests_properties(logic PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=/opt/host-tools/usr/lib/x86_64-linux-gnu/qt6/plugins;LD_LIBRARY_PATH=/opt/host-tools/usr/lib/x86_64-linux-gnu")

# --- multi-process integration harness over real UDP/TLS (UC-01..13) ---
# The same binary runs as scenario driver (default) or as a scripted node
# (--node <name> <datadir>). Nodes communicate line-JSON over stdin/stdout.
# Cross/sandbox note: node subprocesses inherit MESHLINK_LAUNCH_LOADER so the
# driver re-executes them through the toolchain loader (see README).
add_executable(tst_integration tst_integration.cpp)
target_link_libraries(tst_integration PRIVATE meshlink_core)
add_test(NAME integration COMMAND ${MESHLINK_TEST_INTERP} $<TARGET_FILE:tst_integration>)
set_tests_properties(integration PROPERTIES
    TIMEOUT 600
    ENVIRONMENT "LD_LIBRARY_PATH=/opt/host-tools/usr/lib/x86_64-linux-gnu;MESHLINK_LAUNCH_LOADER=/opt/host-tools/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 --library-path /opt/host-tools/usr/lib/x86_64-linux-gnu;QT_PLUGIN_PATH=/opt/host-tools/usr/lib/x86_64-linux-gnu/qt6/plugins")

# --- UI smoke test: pages, DTK settings dialog, button geometry, snapshots ---
if(MESHLINK_BUILD_GUI)
    add_executable(tst_ui_smoke tst_ui_smoke.cpp)
    target_link_libraries(tst_ui_smoke PRIVATE meshlink_ui Dtk6::Widget Dtk6::Gui Qt6::Widgets)
    qt6_add_resources(tst_ui_smoke "ui_smoke_settings"
        PREFIX "/"
        BASE "${PROJECT_SOURCE_DIR}/resources"
        FILES "${PROJECT_SOURCE_DIR}/resources/settings/meshlink-settings.json")
    add_test(NAME ui_smoke COMMAND ${MESHLINK_TEST_INTERP} $<TARGET_FILE:tst_ui_smoke>)
    set_tests_properties(ui_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=/opt/host-tools/usr/lib/x86_64-linux-gnu/qt6/plugins;LD_LIBRARY_PATH=/opt/host-tools/usr/lib/x86_64-linux-gnu"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        TIMEOUT 120)
endif()

# --- translation completeness ---
add_test(NAME translations COMMAND ${CMAKE_COMMAND}
    -DTS_FILE=${PROJECT_SOURCE_DIR}/translations/deepin-meshlink_zh_CN.ts
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_translations.cmake)
