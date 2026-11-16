# deepin-MeshLink — self-tests
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 HuanSoft Open Source Community <https://huansoft.org.cn>

# --- pure-logic unit test (protocol framing, chunk math) ---
add_executable(tst_logic tst_logic.cpp)
target_link_libraries(tst_logic PRIVATE meshlink_core)
add_test(NAME logic COMMAND tst_logic)

# --- multi-process integration harness over real UDP/TLS (UC-01..13) ---
# The same binary runs as scenario driver (default) or as a scripted node
# (--node <name> <datadir>). Nodes communicate line-JSON over stdin/stdout.
add_executable(tst_integration tst_integration.cpp)
target_link_libraries(tst_integration PRIVATE meshlink_core)
add_test(NAME integration COMMAND tst_integration)
set_tests_properties(integration PROPERTIES TIMEOUT 600)
