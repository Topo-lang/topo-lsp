# topo-lsp tests — formatter / LSPServer unit tests + per-bridge integration
# tests + JSON-RPC protocol tests. External LSP servers (clangd, rust-analyzer,
# jdtls, pyright) are runtime dependencies; the integration tests detect them
# at runtime and self-skip if absent.

# MinGW on GHA: cached upstream topo-install may have been linked against
# a different libstdc++/libgcc ABI than C:\mingw64\bin\ currently provides,
# tripping STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139) when gtest_discover_tests
# launches each binary. Static-link the C++ runtime for every test exe in
# this directory so they're hermetic.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ── Formatter unit tests (pure syntax, no plugin dependency) ──
add_executable(topo-lsp-tests unit/FormatterTest.cpp)
target_link_libraries(topo-lsp-tests PRIVATE
    topo::core::TopoFormat
    topo::core::TopoParser
    topo::core::TopoLexer
    topo::core::TopoAST
    topo::core::TopoBasic
    GTest::gtest_main)
target_include_directories(topo-lsp-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/lib)
target_compile_definitions(topo-lsp-tests PRIVATE
    TOPO_TEST_FIXTURES_DIR="${TOPO_CORE_FIXTURES_DIR}")
topo_lsp_set_compiler_flags(topo-lsp-tests)
gtest_discover_tests(topo-lsp-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;lsp" TIMEOUT 60)

# ── LSPServer unit tests ──
# Recompiles LSPServer.cpp directly so test-only seams (e.g. private-member
# friends) work; same transitive plugin reach as the topo-lsp binary.
add_executable(topo-lsp-server-tests
    unit/LSPServerTest.cpp
    ${PROJECT_SOURCE_DIR}/LSPServer.cpp)
target_link_libraries(topo-lsp-server-tests PRIVATE
    TopoLSPLib
    GTest::gtest_main)
target_include_directories(topo-lsp-server-tests PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/lib
    ${TOMLPLUSPLUS_INCLUDE_DIR})
target_compile_definitions(topo-lsp-server-tests PRIVATE
    TOPO_LSP_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/test/fixtures")
topo_lsp_set_compiler_flags(topo-lsp-server-tests)
gtest_discover_tests(topo-lsp-server-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;lsp" TIMEOUT 60)

# ── Fake LSP/JSON-RPC server ──
# Deliberately broken stdio server the *BridgeMalformedResponseHandled tests
# point a real bridge at, exercising the bridge's framing/parse/init path
# against malformed input.
add_executable(topo-lsp-fake-langserver integration/fake_langserver.cpp)
topo_lsp_set_compiler_flags(topo-lsp-fake-langserver)
set(TOPO_LSP_FAKE_LANGSERVER "$<TARGET_FILE:topo-lsp-fake-langserver>")

# ── Per-bridge integration tests ──
# Each integration test links its language's LSP bridge target (TopoCppLSP /
# TopoRustLSP / TopoJavaLSP / TopoPythonLSP — TypeScript's TsServerBridge
# lives in topo-v8 and is exercised through topo-v8 directly). Each is built
# only when the corresponding plugin was resolved.

# Helper: each entry maps lang-lower → bridge target → cpp source → timeout.
# C++:    topo::lang-cpp::TopoCppLSP
# Rust:   topo::lang-rust::TopoRustLSP
# Java:   topo::lang-java::TopoJavaLSP
# Python: topo::lang-python::TopoPythonLSP
# (TypeScript uses topo::v8::TopoV8TsServer, exercised via topo-v8 tests.)
function(_topo_lsp_add_bridge_integration_test lang_lower lang_pascal source timeout_sec)
    # Per-host LSP bridges (TopoCppLSP / TopoRustLSP / TopoJavaLSP / TopoPythonLSP)
    # ship in the upstream lang-* package's default install set even when the
    # plugin aggregator is gated OFF (cpp/rust LLVM-scope deferred). Ensure
    # the package is loaded so its bridge target is imported.
    if(NOT TARGET topo::lang-${lang_lower}::Topo${lang_pascal}LSP)
        find_package(topo-lang-${lang_lower} CONFIG QUIET)
    endif()
    if(NOT TARGET topo::lang-${lang_lower}::Topo${lang_pascal}LSP)
        message(STATUS "topo-lsp: ${lang_lower} bridge integration test skipped (target topo::lang-${lang_lower}::Topo${lang_pascal}LSP not imported)")
        return()
    endif()
    set(_tname topo-lsp-${lang_lower}-integration-tests)
    if(lang_lower STREQUAL "cpp")
        set(_tname topo-lsp-integration-tests)  # historical name
    endif()
    # Fixture-directory routing: clangd_test lives in topo-core's installed
    # fixture set (used by other topo-core checker e2es); rust_analyzer_test /
    # jdt_test / pyright_test live in this repo's own test/fixtures/.
    if(lang_lower STREQUAL "cpp")
        set(_fixtures_dir "${TOPO_CORE_FIXTURES_DIR}")
    else()
        set(_fixtures_dir "${PROJECT_SOURCE_DIR}/test/fixtures")
    endif()
    add_executable(${_tname} ${source})
    add_dependencies(${_tname} topo-lsp-fake-langserver)
    target_link_libraries(${_tname} PRIVATE
        topo::lang-${lang_lower}::Topo${lang_pascal}LSP
        GTest::gtest_main)
    target_compile_definitions(${_tname} PRIVATE
        TOPO_TEST_FIXTURES_DIR="${_fixtures_dir}"
        TOPO_LSP_FAKE_LANGSERVER="${TOPO_LSP_FAKE_LANGSERVER}")
    topo_lsp_set_compiler_flags(${_tname})
    add_test(NAME ${_tname} COMMAND ${_tname})
    set_tests_properties(${_tname} PROPERTIES LABELS "toolchain;lsp" TIMEOUT ${timeout_sec})
endfunction()

# Bridge integration tests spawn external LSP servers (clangd /
# rust-analyzer / jdtls / pyright) and exchange real JSON-RPC. They run
# on every platform, Windows included: the platform spawn layer uses a
# raw Win32 backend there and resolves bare server names against PATH —
# including .cmd/.bat launchers (jdtls.bat, pyright-langserver.cmd),
# which it routes through cmd.exe. Suites self-skip per test when the
# corresponding server is not installed.
_topo_lsp_add_bridge_integration_test(cpp    Cpp    integration/ClangdBridgeTest.cpp       180)
_topo_lsp_add_bridge_integration_test(rust   Rust   integration/RustAnalyzerBridgeTest.cpp 120)
_topo_lsp_add_bridge_integration_test(java   Java   integration/JdtBridgeTest.cpp          300)
_topo_lsp_add_bridge_integration_test(python Python integration/PyrightBridgeTest.cpp      120)

# ── LSP protocol tests ──
# Spawn the real topo-lsp binary as a subprocess and exchange framed JSON-RPC
# messages over stdin/stdout pipes (PipedProcess — raw Win32 on Windows).
add_executable(topo-lsp-protocol-tests
    protocol/LspProtocolHandshakeTest.cpp
    protocol/LspSyncNotificationsTest.cpp
    protocol/LspRequestsTest.cpp)
target_link_libraries(topo-lsp-protocol-tests PRIVATE
    topo::core::TopoPlatform
    nlohmann_json::nlohmann_json
    GTest::gtest_main)
target_include_directories(topo-lsp-protocol-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
target_compile_definitions(topo-lsp-protocol-tests PRIVATE
    TOPO_LSP_BINARY="$<TARGET_FILE:topo-lsp>")
add_dependencies(topo-lsp-protocol-tests topo-lsp)
topo_lsp_set_compiler_flags(topo-lsp-protocol-tests)
gtest_discover_tests(topo-lsp-protocol-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;lsp-protocol" TIMEOUT 60)

# ── Python-driven end-to-end LSP tests ──
# The scripts spawn topo-lsp via TOPO_LSP_EXE and speak framed JSON-RPC over
# plain subprocess pipes; both are platform-portable (exe suffix, drive-letter
# file:/// URIs).
find_package(Python3 QUIET)
if(Python3_FOUND)
    add_test(NAME lsp-integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_lsp.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    add_test(NAME lsp-demo-integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_lsp_demo.py
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    # PYTHONIOENCODING: the drivers print unicode (e.g. arrows) in their
    # progress banners; the Windows console default is a legacy codepage
    # (cp1252) and charmap_encode aborts the whole test on the first
    # non-encodable character. Force UTF-8 stdout everywhere.
    set_tests_properties(lsp-integration lsp-demo-integration
        PROPERTIES
            LABELS "lsp"
            TIMEOUT 60
            ENVIRONMENT
                "TOPO_LSP_EXE=$<TARGET_FILE:topo-lsp>;PYTHONIOENCODING=utf-8")
endif()
