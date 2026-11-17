# deepin-MeshLink (深度织讯) — LAN decentralised IM client
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>
#
# DTK6 + Qt6 build. Targets deepin V25 / UOS V25.
cmake_minimum_required(VERSION 3.16)

project(deepin-meshlink
    VERSION 26.4.0.0
    DESCRIPTION "LAN decentralised instant-messaging client (deepin-MeshLink)"
    HOMEPAGE_URL "https://huansoft.org.cn"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MESHLINK_BUILD_TESTS "Build MeshLink self-tests" ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql Concurrent DBus)
find_package(Dtk6 REQUIRED COMPONENTS Core Gui Widget)
find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)

if(DEFINED OpenSSL_VERSION AND NOT OpenSSL_VERSION STREQUAL "" AND OpenSSL_VERSION VERSION_LESS "1.1.1")
    message(FATAL_ERROR "OpenSSL >= 1.1.1 required for TLS 1.3 (found ${OpenSSL_VERSION})")
endif()

message(STATUS "MeshLink: Qt ${Qt6_VERSION}, DTK ${Dtk6_VERSION}, OpenSSL ${OpenSSL_VERSION}")

# Host/cross toolchains may need an emulator wrapper for the Qt host tools
# (rcc/lrelease). Empty by default so native deepin builds are untouched.
set(MESHLINK_RCC_EXECUTABLE "" CACHE FILEPATH
    "Optional rcc wrapper/emulator for cross or sandbox builds")
if(MESHLINK_RCC_EXECUTABLE AND TARGET Qt6::rcc)
    set_target_properties(Qt6::rcc PROPERTIES
        IMPORTED_LOCATION "${MESHLINK_RCC_EXECUTABLE}")
endif()

# Debian multiarch: opensslconf.h lives under /usr/include/<triplet>/openssl.
# (Also needed with mirrored host toolchains, e.g. /opt/host-tools/usr/include.)
get_target_property(MESHLINK_OPENSSL_INC OpenSSL::Crypto INTERFACE_INCLUDE_DIRECTORIES)
unset(MESHLINK_OPENSSL_ARCH_INC)
find_path(MESHLINK_OPENSSL_ARCH_INC openssl/opensslconf.h
    PATHS "${MESHLINK_OPENSSL_INC}/${CMAKE_LIBRARY_ARCHITECTURE}"
          "${MESHLINK_OPENSSL_INC}/../${CMAKE_LIBRARY_ARCHITECTURE}"
          "/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}"
    NO_DEFAULT_PATH)
unset(MESHLINK_OPENSSL_INC)

# ---------------------------------------------------------------------------
# meshlink_core : Qt-only application/network/crypto logic (no DTK Widgets),
# so it can be unit/integration tested headlessly.
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    src/core/crypto.cpp
    src/core/protocol.cpp
    src/core/account.cpp
    src/core/storage.cpp
    src/core/settings.cpp
    src/core/lansegment.cpp
    src/core/discovery.cpp
    src/core/securelink.cpp
    src/core/roommanager.cpp
    src/core/chatmanager.cpp
    src/core/filemanager.cpp
    src/core/downloader.cpp
    src/core/engine.cpp
)
set(CORE_HEADERS
    src/core/common.h
    src/core/crypto.h
    src/core/notifypolicy.h
    src/core/protocol.h
    src/core/account.h
    src/core/storage.h
    src/core/settings.h
    src/core/lansegment.h
    src/core/discovery.h
    src/core/securelink.h
    src/core/roommanager.h
    src/core/chatmanager.h
    src/core/filemanager.h
    src/core/downloader.h
    src/core/engine.h
    src/core/chunkplan.h
    src/core/tlsutil.h
)

add_library(meshlink_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})
target_include_directories(meshlink_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MESHLINK_OPENSSL_ARCH_INC)
    target_include_directories(meshlink_core PUBLIC "${MESHLINK_OPENSSL_ARCH_INC}")
endif()
target_link_libraries(meshlink_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::Concurrent
    Dtk6::Core
    OpenSSL::Crypto OpenSSL::SSL)
target_compile_definitions(meshlink_core PUBLIC
    MESHLINK_VERSION="${PROJECT_VERSION}"
    MESHLINK_STAGE="alpha")

# ---------------------------------------------------------------------------
# meshlink_ui : DTK6 pages shared by the application and the UI smoke test.
# ---------------------------------------------------------------------------
option(MESHLINK_BUILD_GUI "Build the DTK6 GUI application" ON)
if(MESHLINK_BUILD_GUI)
set(UI_SOURCES
    src/ui/uiutils.cpp
    src/ui/appicon.cpp
    src/ui/mainwindow.cpp
    src/ui/profilepage.cpp
    src/ui/roomsview.cpp
    src/ui/chatview.cpp
    src/ui/discoverview.cpp
    src/ui/transfersview.cpp
    src/ui/settingsdialog.cpp
    src/ui/settingsi18n.cpp
    src/ui/firststartwizard.cpp
    src/ui/xdgnotifications.cpp
    src/ui/notificationcenter.cpp
    src/ui/xdgtrayicon.cpp
)
set(UI_HEADERS
    src/ui/uiutils.h
    src/ui/appicon.h
    src/ui/mainwindow.h
    src/ui/profilepage.h
    src/ui/roomsview.h
    src/ui/chatview.h
    src/ui/discoverview.h
    src/ui/transfersview.h
    src/ui/settingsdialog.h
    src/ui/firststartwizard.h
    src/ui/xdgnotifications.h
    src/ui/notificationcenter.h
    src/ui/xdgtrayicon.h
)

add_library(meshlink_ui STATIC ${UI_SOURCES} ${UI_HEADERS})
target_link_libraries(meshlink_ui PUBLIC
    meshlink_core Dtk6::Widget Dtk6::Gui Qt6::Widgets Qt6::DBus)
target_include_directories(meshlink_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# deepin-meshlink : the DTK6 executable
# ---------------------------------------------------------------------------
add_executable(deepin-meshlink src/app/main.cpp)
target_link_libraries(deepin-meshlink PRIVATE meshlink_ui)

qt6_add_resources(deepin-meshlink "meshlink_settings"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/settings/meshlink-settings.json")

# ---------------------------------------------------------------------------
# Translations: English source strings + zh_CN language pack.
# `update_translations` refreshes the .ts; normal builds only run lrelease.
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
# Host/cross toolchains may need an emulator wrapper for Qt's lrelease binary
# (the sandbox uses the host loader). Empty by default for native builds.
set(MESHLINK_LRELEASE_EXECUTABLE "" CACHE FILEPATH
    "Optional lrelease wrapper/emulator for cross or sandbox builds")
if(MESHLINK_LRELEASE_EXECUTABLE)
    set_target_properties(Qt6::lrelease PROPERTIES
        IMPORTED_LOCATION "${MESHLINK_LRELEASE_EXECUTABLE}")
endif()
set(MESHLINK_TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/translations/deepin-meshlink_zh_CN.ts")
qt6_add_lupdate(TS_FILES ${MESHLINK_TS_FILES}
    SOURCE_TARGETS deepin-meshlink meshlink_ui meshlink_core)
qt6_add_lrelease(TS_FILES ${MESHLINK_TS_FILES}
    LRELEASE_TARGET meshlink_lrelease
    QM_FILES_OUTPUT_VARIABLE MESHLINK_QM_FILES)
add_dependencies(deepin-meshlink meshlink_lrelease)

if(MESHLINK_QM_FILES)
    add_custom_command(TARGET deepin-meshlink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/translations"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MESHLINK_QM_FILES} "${CMAKE_BINARY_DIR}/translations/")
endif()

install(TARGETS deepin-meshlink RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/cn.org.huansoft.deepin-meshlink.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY resources/icons/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        FILES_MATCHING PATTERN "*.svg")
install(FILES resources/meshlink.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME cn.org.huansoft.deepin-meshlink.png)
if(MESHLINK_QM_FILES)
    install(FILES ${MESHLINK_QM_FILES}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/deepin-meshlink/translations)
endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MESHLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
