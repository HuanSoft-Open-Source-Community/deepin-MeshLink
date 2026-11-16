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
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql Concurrent)
find_package(Dtk6 REQUIRED COMPONENTS Core Gui Widget)
find_package(OpenSSL REQUIRED COMPONENTS Crypto SSL)

if(DEFINED OpenSSL_VERSION AND NOT OpenSSL_VERSION STREQUAL "" AND OpenSSL_VERSION VERSION_LESS "1.1.1")
    message(FATAL_ERROR "OpenSSL >= 1.1.1 required for TLS 1.3 (found ${OpenSSL_VERSION})")
endif()

message(STATUS "MeshLink: Qt ${Qt6_VERSION}, DTK ${Dtk6_VERSION}, OpenSSL ${OpenSSL_VERSION}")

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
# deepin-meshlink : the DTK6 GUI application
# ---------------------------------------------------------------------------
option(MESHLINK_BUILD_GUI "Build the DTK6 GUI application" ON)
if(MESHLINK_BUILD_GUI)
set(UI_SOURCES
    src/ui/appicon.cpp
    src/ui/mainwindow.cpp
    src/ui/profilepage.cpp
    src/ui/roomsview.cpp
    src/ui/chatview.cpp
    src/ui/discoverview.cpp
    src/ui/transfersview.cpp
    src/ui/settingsview.cpp
    src/ui/firststartwizard.cpp
)
set(UI_HEADERS
    src/ui/appicon.h
    src/ui/mainwindow.h
    src/ui/profilepage.h
    src/ui/roomsview.h
    src/ui/chatview.h
    src/ui/discoverview.h
    src/ui/transfersview.h
    src/ui/settingsview.h
    src/ui/firststartwizard.h
)

add_executable(deepin-meshlink src/app/main.cpp ${UI_SOURCES} ${UI_HEADERS})
target_link_libraries(deepin-meshlink PRIVATE meshlink_core Dtk6::Widget Dtk6::Gui Qt6::Widgets)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MESHLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install (used by debian/rules via `dh $@ --buildsystem=cmake`)
# ---------------------------------------------------------------------------
if(MESHLINK_BUILD_GUI)
install(TARGETS deepin-meshlink RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/cn.org.huansoft.deepin-meshlink.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY resources/icons/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        FILES_MATCHING PATTERN "*.svg")
install(FILES resources/meshlink.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME cn.org.huansoft.deepin-meshlink.png)
endif()
