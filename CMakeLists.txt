cmake_minimum_required(VERSION 3.20)

project(topo-lsp
    VERSION 0.1.0
    DESCRIPTION "Topo LSP server — JSON-RPC LSP frontend dispatching to per-host language bridges (clangd / rust-analyzer / jdtls / pyright / tsserver)"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-lsp"
    LANGUAGES CXX
)

# topo-lsp: the JSON-RPC LSP server consuming topo-core + topo-lang and any
# subset of the 5 topo-lang-* plugins (cpp / rust / java / python / typescript).
# Each plugin's per-host LSP bridge (ClangdBridge / RustAnalyzerBridge /
# JdtBridge / PyrightBridge / TsServerBridge) is wired in only when the
# matching upstream plugin package is installed and exports its plugin
# aggregator target.
#
# Plugin availability is auto-detected: each TOPO_LSP_ENABLE_<LANG>_PLUGIN
# defaults ON, but if find_package(topo-lang-<lang>) fails or the package
# does not export the plugin aggregator target (e.g. the LLVM-scope-gated
# TopoCppPlugin / TopoRustPlugin), the option is forcibly disabled and the
# plugin is omitted from the build. main.cpp uses TOPO_LSP_WITH_<LANG>_PLUGIN
# compile definitions to conditionally compile in the corresponding
# topo::lang::register<Lang>Plugin() registration call.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TopoLspCompilerFlags)

# ── Upstream Topo packages ──────────────────────────────
# Guards let topo-lsp be add_subdirectory()'d from a meta-repo that has
# already brought in topo-core / topo-lang as in-tree targets — the
# find_package() then would fail with "could not find target X" since
# the sentinels already exist. Standalone build: sentinel missing →
# find_package runs as normal.
if(NOT TARGET topo::core::TopoBasic)
    find_package(topo-core CONFIG REQUIRED)
endif()
if(NOT TARGET topo::lang::TopoLang)
    find_package(topo-lang CONFIG REQUIRED)
endif()

# ── External dependencies ───────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)
# toml++ is fetched header-only (same as topo-build) — avoids RTTI conflict.
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

# ── Plugin auto-detection ───────────────────────────────
option(TOPO_LSP_ENABLE_CPP_PLUGIN        "Link topo::lang-cpp::TopoCppPlugin"               ON)
option(TOPO_LSP_ENABLE_RUST_PLUGIN       "Link topo::lang-rust::TopoRustPlugin"             ON)
option(TOPO_LSP_ENABLE_JAVA_PLUGIN       "Link topo::lang-java::TopoJavaPlugin"             ON)
option(TOPO_LSP_ENABLE_PYTHON_PLUGIN     "Link topo::lang-python::TopoPythonPlugin"         ON)
option(TOPO_LSP_ENABLE_TYPESCRIPT_PLUGIN "Link topo::lang-typescript::TopoTypeScriptPlugin" ON)

# Each plugin entry: <lang-lower>:<LANG_UPPER>:<LangPascal>
# capitalization matches the Topo<Pascal>Plugin target name (e.g. TopoCppPlugin
# not TopoCPPPlugin, TopoTypeScriptPlugin not TopoTYPESCRIPTPlugin).
set(_TOPO_LSP_PLUGIN_LIST
    "cpp:CPP:Cpp"
    "rust:RUST:Rust"
    "java:JAVA:Java"
    "python:PYTHON:Python"
    "typescript:TYPESCRIPT:TypeScript"
)

# topo-v8 is needed when the TypeScript plugin is in scope (TsServerBridge
# lives in topo-v8/lib/TsServer). Find it conditionally so a build without
# the TS plugin doesn't require topo-v8.
if(TOPO_LSP_ENABLE_TYPESCRIPT_PLUGIN)
    if(NOT TARGET topo::v8::Topov8Placeholder)
        find_package(topo-v8 CONFIG QUIET)
    endif()
    if(NOT TARGET topo::v8::Topov8Placeholder AND NOT topo-v8_FOUND)
        message(STATUS "topo-lsp: topo-v8 not found; disabling TypeScript plugin (TsServerBridge lives there)")
        set(TOPO_LSP_ENABLE_TYPESCRIPT_PLUGIN OFF CACHE BOOL "" FORCE)
    endif()
endif()

set(_TOPO_LSP_RESOLVED_PLUGINS "")
foreach(entry IN LISTS _TOPO_LSP_PLUGIN_LIST)
    string(REPLACE ":" ";" _parts "${entry}")
    list(GET _parts 0 _lang_lower)
    list(GET _parts 1 _lang_upper)
    list(GET _parts 2 _lang_pascal)

    if(NOT TOPO_LSP_ENABLE_${_lang_upper}_PLUGIN)
        message(STATUS "topo-lsp: ${_lang_lower} plugin disabled by option")
        continue()
    endif()

    set(_plugin_target "topo::lang-${_lang_lower}::Topo${_lang_pascal}Plugin")
    if(NOT TARGET ${_plugin_target})
        find_package(topo-lang-${_lang_lower} CONFIG QUIET)
    endif()
    if(NOT TARGET ${_plugin_target} AND NOT topo-lang-${_lang_lower}_FOUND)
        message(STATUS "topo-lsp: topo-lang-${_lang_lower} not found; disabling ${_lang_lower} plugin")
        set(TOPO_LSP_ENABLE_${_lang_upper}_PLUGIN OFF CACHE BOOL "" FORCE)
        continue()
    endif()

    if(NOT TARGET ${_plugin_target})
        message(STATUS "topo-lsp: topo-lang-${_lang_lower} found but ${_plugin_target} target missing (likely backend-scope-gated OFF upstream); disabling ${_lang_lower} plugin")
        set(TOPO_LSP_ENABLE_${_lang_upper}_PLUGIN OFF CACHE BOOL "" FORCE)
        continue()
    endif()

    message(STATUS "topo-lsp: ${_lang_lower} plugin will be linked (${_plugin_target})")
    list(APPEND _TOPO_LSP_RESOLVED_PLUGINS "${_lang_lower}:${_lang_upper}:${_lang_pascal}")
endforeach()

if(NOT _TOPO_LSP_RESOLVED_PLUGINS)
    message(WARNING "topo-lsp: no language plugins resolved; server will run with empty plugin registry (only frontend diagnostics will work)")
endif()

# ── TopoLSPLib (LSP server library) + topo-lsp executable ──────
# Library form is exported so downstream tools (topo-cli's `topo lsp`
# subcommand, embedded LSP hosts) can link it.
add_library(TopoLSPLib STATIC
    LSPServer.cpp
    lib/Formatter.cpp
)
# In-tree alias mirroring the post-install exported namespace, so callers
# using `topo::lsp::TopoLSPLib` work both standalone (this alias) and as
# an installed package (the EXPORT below).
add_library(topo::lsp::TopoLSPLib ALIAS TopoLSPLib)

# Each lang plugin PUBLIC-propagates topo::lang::TopoLang and its
# per-language LSP bridge, which PUBLIC-depends on topo::core::TopoLSPBridge
# — so TopoLSPBridge / TopoLang transitively reach TopoLSPLib via the
# plugins. We also link the topo-core libs LSPServer.cpp directly uses.
target_link_libraries(TopoLSPLib PUBLIC
    topo::core::TopoBasic
    topo::core::TopoAST
    topo::core::TopoSema
    topo::core::TopoLSPBridge
    topo::lang::TopoLang
    nlohmann_json::nlohmann_json
)
target_link_libraries(TopoLSPLib PRIVATE
    topo::core::TopoLexer
    topo::core::TopoParser
    topo::core::TopoStdlib
    topo::core::TopoAnalysis
    topo::core::TopoFormat
    topo::core::TopoTransform
    topo::core::TopoBuildConfig
    topo::core::TopoBuildLib
    topo::core::TopoPlatform
)

# Conditionally link each resolved plugin and define the matching
# TOPO_LSP_WITH_<LANG>_PLUGIN compile def (used by main.cpp).
foreach(entry IN LISTS _TOPO_LSP_RESOLVED_PLUGINS)
    string(REPLACE ":" ";" _parts "${entry}")
    list(GET _parts 0 _lang_lower)
    list(GET _parts 1 _lang_upper)
    list(GET _parts 2 _lang_pascal)
    target_link_libraries(TopoLSPLib PUBLIC
        topo::lang-${_lang_lower}::Topo${_lang_pascal}Plugin)
    target_compile_definitions(TopoLSPLib PUBLIC TOPO_LSP_WITH_${_lang_upper}_PLUGIN=1)
endforeach()

target_include_directories(TopoLSPLib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-lsp>
    PRIVATE
        ${TOMLPLUSPLUS_INCLUDE_DIR}
)
topo_lsp_set_compiler_flags(TopoLSPLib)

# Per-plugin LSP bridges (TopoCppLSP / TopoRustLSP / TopoJavaLSP /
# TopoPythonLSP / TopoV8TsServer) reach LSPServer.cpp transitively via the
# plugin aggregators — no separate link needed for the topo-lsp binary.

add_executable(topo-lsp main.cpp)
target_link_libraries(topo-lsp PRIVATE TopoLSPLib)
topo_lsp_set_compiler_flags(topo-lsp)

# ── Install / export ──────────────────────────────────
install(TARGETS TopoLSPLib topo-lsp
    EXPORT topo-lspTargets
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lsp
)

# Install the public headers under include/topo-lsp/ so downstream
# consumers can #include "LSPServer.h" via $<INSTALL_INTERFACE:...>.
install(FILES LSPServer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lsp)
install(FILES lib/Formatter.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-lsp)

install(EXPORT topo-lspTargets
    FILE topo-lspTargets.cmake
    NAMESPACE topo::lsp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-lsp
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/topo-lspConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lspConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-lsp
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lspConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lspConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/topo-lspConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-lsp
)

# ── Tests ─────────────────────────────────────────────
option(TOPO_LSP_BUILD_TESTS "Build topo-lsp tests" ON)
if(TOPO_LSP_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR TOPO_META_DRIVING_TESTS))
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    include(GoogleTest)
    add_subdirectory(test)
endif()
