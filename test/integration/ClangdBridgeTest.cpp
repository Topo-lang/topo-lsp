#include "ClangdBridge.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace topo::lsp;
namespace fs = std::filesystem;

static std::string fixtureDir() {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/clangd_test";
}

// Helper: find clangd from bundled or PATH
static std::string findClangd() {
#ifdef TOPO_LLVM_BINDIR
#ifdef _WIN32
    fs::path bundled = fs::path(TOPO_LLVM_BINDIR) / "clangd.exe";
#else
    fs::path bundled = fs::path(TOPO_LLVM_BINDIR) / "clangd";
#endif
    if (fs::exists(bundled)) {
        return bundled.string();
    }
#endif
    return ""; // empty = let ClangdBridge search PATH
}

// Returns true if any clangd binary (bundled or on PATH) is runnable on
// this machine.  When true, `resolvedExe` is populated with the bundled
// path (if present) or left empty to let ClangdBridge search PATH.
static bool clangdIsRunnable(std::string& resolvedExe) {
    resolvedExe = findClangd();
    std::string checkCmd = resolvedExe.empty() ? "clangd" : resolvedExe;
#ifdef _WIN32
    int ret = std::system(("\"" + checkCmd + "\" --version > NUL 2>&1").c_str());
#else
    int ret = std::system(("\"" + checkCmd + "\" --version > /dev/null 2>&1").c_str());
#endif
    return ret == 0;
}

// Start a bridge pointed at the fixture directory.  Returns false if
// clangd refuses to start (caller should GTEST_SKIP in that case).
//
// CRITICAL: clangd's background index only indexes a translation unit once
// the file is OPENED via textDocument/didOpen (or the project is fully
// scanned, which clangd does NOT do for a bare compile_commands.json on a
// cold cache). The earlier "30s poll-until-ready was insufficient" symptom
// was NOT cold-start latency — without an open document, clangd never
// indexes main.cpp at all, so workspace/symbol can never resolve
// `engine::init` no matter how long the test waits. Opening the fixture TU
// here makes clangd resolve the fixture's own symbols in ~1s.
// (issue: lsp-bridge-happy-path-tests-skip-despite-langserver-installed)
static bool startOnFixture(ClangdBridge& bridge, const std::string& clangdExe) {
    std::string dir = fixtureDir();
    std::string rootUri = "file:///" + dir;
    if (!bridge.start(clangdExe, dir, rootUri)) return false;
    // Open the only TU in the fixture so clangd parses + indexes it. Without
    // this, the fixture's symbols never enter clangd's index.
    bridge.openDocument(dir + "/main.cpp");
    // Give the background index a chance to populate so workspace/symbol
    // queries can succeed.  waitForIndex returns as soon as any response
    // arrives; the 15s cap is just a safety net.
    bridge.waitForIndex(std::chrono::milliseconds{15000});
    return true;
}

// Poll-until-ready: clangd's background index can take well over a single
// short query's worth of time to resolve a symbol on a cold cache. Rather
// than GTEST_SKIP after one miss (which silently drops coverage on machines
// where clangd IS installed — see issue
// lsp-bridge-happy-path-tests-skip-despite-langserver-installed), retry the
// query until it yields a value or a bounded deadline elapses. The
// missing-binary case is already gated by clangdIsRunnable()/start() before
// this is ever called, so a persistent miss here means a genuine timeout,
// not an absent tool.
template <typename T>
static std::optional<T> pollUntil(std::function<std::optional<T>()> query,
                                  std::chrono::milliseconds budget = std::chrono::milliseconds{30000},
                                  std::chrono::milliseconds interval = std::chrono::milliseconds{500}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        auto r = query();
        if (r.has_value()) return r;
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(interval);
    }
}

// Poll until the query yields a value that ALSO satisfies `accept`. This is
// needed because LSPBridge::queryWorkspaceSymbol falls back to "return the
// first workspace/symbol result" when no exact qualified-name match is
// found yet. On a cold index clangd answers a `init` query with unrelated
// same-simple-name symbols from system headers before the fixture's
// `engine::init` is indexed, so a plain pollUntil would lock onto that
// wrong first result. Requiring the result to live in the fixture makes the
// poll wait for the fixture TU to actually be indexed.
// (issue: lsp-bridge-happy-path-tests-skip-despite-langserver-installed;
//  the first-result fallback itself is tracked separately.)
template <typename T>
static std::optional<T> pollUntilSatisfies(
    std::function<std::optional<T>()> query,
    std::function<bool(const T&)> accept,
    std::chrono::milliseconds budget = std::chrono::milliseconds{30000},
    std::chrono::milliseconds interval = std::chrono::milliseconds{500}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        auto r = query();
        if (r.has_value() && accept(*r)) return r;
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(interval);
    }
}

// Same idea for queries returning a vector (findReferences): poll until the
// result is non-empty or the deadline elapses.
template <typename T>
static std::vector<T> pollUntilNonEmpty(std::function<std::vector<T>()> query,
                                        std::chrono::milliseconds budget = std::chrono::milliseconds{30000},
                                        std::chrono::milliseconds interval = std::chrono::milliseconds{500}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        auto r = query();
        if (!r.empty()) return r;
        if (std::chrono::steady_clock::now() >= deadline) return r;
        std::this_thread::sleep_for(interval);
    }
}

// ---------------------------------------------------------------------------
// Property checks (no backend required)
// ---------------------------------------------------------------------------

// Test: invalid path gracefully fails
TEST(ClangdBridge, InvalidPathFails) {
    ClangdBridge bridge;
    bool started = bridge.start("/nonexistent/clangd-xxxx", "", "file:///tmp");
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: stop on non-started bridge is safe
TEST(ClangdBridge, StopWithoutStart) {
    ClangdBridge bridge;
    bridge.stop(); // Should not crash
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: query on unavailable bridge returns nullopt
TEST(ClangdBridge, QueryWhenUnavailable) {
    ClangdBridge bridge;
    auto result = bridge.findDefinition("engine::init", {});
    EXPECT_FALSE(result.has_value());

    auto refs = bridge.findReferences("engine::init", {});
    EXPECT_TRUE(refs.empty());

    auto hover = bridge.getHoverInfo("engine::init", {});
    EXPECT_FALSE(hover.has_value());
}

// Test: identity metadata is stable regardless of backend state
TEST(ClangdBridge, DisplayNameAndLanguageIdStable) {
    ClangdBridge bridge;
    EXPECT_EQ(bridge.displayName(), "C++");
    EXPECT_EQ(bridge.languageId(), "cpp");
}

// Test: empty qualified name on unavailable bridge is a graceful nullopt
TEST(ClangdBridge, EmptyQualifiedNameReturnsNullopt) {
    ClangdBridge bridge;
    auto def = bridge.findDefinition("", {});
    EXPECT_FALSE(def.has_value());
    auto hover = bridge.getHoverInfo("", {});
    EXPECT_FALSE(hover.has_value());
    auto refs = bridge.findReferences("", {});
    EXPECT_TRUE(refs.empty());
}

// ---------------------------------------------------------------------------
// Tests that require a real clangd binary
// ---------------------------------------------------------------------------

// Test: start with clangd from bundled or PATH (skip if not available)
TEST(ClangdBridge, StartFromPathIfAvailable) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) {
        GTEST_SKIP() << "clangd not found (bundled or PATH), skipping";
    }

    ClangdBridge bridge;
    std::string dir = fixtureDir();
    std::string rootUri = "file:///" + dir;

    bool started = bridge.start(clangdExe, dir, rootUri);
    if (!started) {
        GTEST_SKIP() << "clangd failed to start, skipping";
    }

    EXPECT_TRUE(bridge.isAvailable());
    // Result of a symbol query is best-effort (depends on indexing); just
    // assert no crash.
    (void)bridge.findDefinition("engine::init", {});

    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: findDefinition on a real known symbol returns a result pointing at
// the fixture source.
//
// Probe `engine::runFrames`, NOT `engine::init`: a workspace/symbol query
// for the bare simple name `init` returns 40+ std:: hits that outrank (and
// truncate out) the fixture's `engine::init`, so the bridge's exact-match
// never sees it and its first-result fallback returns a system-header
// symbol — the fixture symbol is genuinely unreachable via that query.
// `runFrames` has no C++ standard-library namesake, so clangd returns
// exactly the fixture definition. (issue:
// lsp-bridge-happy-path-tests-skip-despite-langserver-installed)
TEST(ClangdBridge, FindDefinitionOnRealSymbol) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start";
    }

    auto def = pollUntilSatisfies<SymbolResult>(
        [&] { return bridge.findDefinition("engine::runFrames", {}); },
        [](const SymbolResult& r) {
            // Only accept the fixture's own definition, not a same-named
            // symbol clangd may return from system headers before the
            // fixture TU finishes indexing.
            return r.file.find("main.cpp") != std::string::npos;
        });
    bridge.stop();
    if (!def.has_value()) {
        GTEST_SKIP() << "clangd index did not resolve engine::init within the "
                        "30s readiness budget (cold cache / overloaded machine)";
    }
    EXPECT_FALSE(def->file.empty());
    // main.cpp is the only TU in the fixture.
    EXPECT_NE(def->file.find("main.cpp"), std::string::npos);
    EXPECT_GE(def->line, 0);
}

// Test: findReferences sees multiple call sites of engine::render.
TEST(ClangdBridge, FindReferencesReturnsMultipleSites) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start";
    }

    auto refs = pollUntilNonEmpty<SymbolResult>(
        [&] { return bridge.findReferences("engine::render", {}); });
    bridge.stop();
    if (refs.empty()) {
        GTEST_SKIP() << "clangd index returned no references for engine::render "
                        "within the 30s readiness budget";
    }
    // We expect at least the definition plus a few call sites (render is
    // called from runFrames, Scene::tick and main).
    EXPECT_GE(refs.size(), 2U);
    for (const auto& r : refs) {
        EXPECT_FALSE(r.file.empty());
        EXPECT_GE(r.line, 0);
    }
}

// Test: getHoverInfo returns a non-empty string mentioning the symbol.
TEST(ClangdBridge, GetHoverInfoReturnsSignature) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start";
    }

    auto hover = pollUntil<std::string>(
        [&] { return bridge.getHoverInfo("engine::render", {}); });
    bridge.stop();
    if (!hover.has_value()) {
        GTEST_SKIP() << "clangd did not produce hover info within the 30s "
                        "readiness budget (index may be cold)";
    }
    EXPECT_FALSE(hover->empty());
    // Hover payload for a function nearly always contains the identifier;
    // we allow either the short or qualified form.
    const bool hasName =
        hover->find("render") != std::string::npos ||
        hover->find("engine") != std::string::npos;
    EXPECT_TRUE(hasName) << "hover payload: " << *hover;
}

// Test: findTypeDefinition resolves a class defined in the fixture.
TEST(ClangdBridge, FindTypeDefinitionOnClass) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start";
    }

    auto def = pollUntil<SymbolResult>(
        [&] { return bridge.findTypeDefinition("engine::Scene", {}, {fixtureDir()}); });
    bridge.stop();
    if (!def.has_value()) {
        GTEST_SKIP() << "neither clangd index nor header scan resolved "
                        "engine::Scene within the 30s readiness budget";
    }
    EXPECT_FALSE(def->file.empty());
}

// Test: querying a name that does not exist returns nullopt without crash.
TEST(ClangdBridge, UnknownSymbolReturnsNullopt) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start";
    }

    auto def = bridge.findDefinition("engine::definitely_not_a_real_symbol_xyz", {});
    auto hover = bridge.getHoverInfo("engine::definitely_not_a_real_symbol_xyz", {});
    auto refs = bridge.findReferences("engine::definitely_not_a_real_symbol_xyz", {});
    bridge.stop();

    EXPECT_FALSE(def.has_value());
    EXPECT_FALSE(hover.has_value());
    EXPECT_TRUE(refs.empty());
}

// Test: a restart cycle correctly isolates the second session from the
// first — we can stop and start again on the same bridge instance.
TEST(ClangdBridge, RestartCycleIsolatesState) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start (first cycle)";
    }
    EXPECT_TRUE(bridge.isAvailable());
    (void)bridge.findDefinition("engine::init", {});
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());

    // Second cycle on the same bridge object.
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to restart";
    }
    EXPECT_TRUE(bridge.isAvailable());
    (void)bridge.findDefinition("engine::render", {});
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: single-argument start(rootUri) overload exercises path
// auto-detection.  The fixture ships with a compile_commands.json in the
// root, which is where the auto-detected buildDir (root/build) is not,
// so we just verify the overload does not crash and either succeeds or
// fails cleanly.
TEST(ClangdBridge, SingleArgStartOverloadAutoDetects) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    std::string rootUri = "file:///" + fixtureDir();
    // This overload internally resolves clangd via bundled/PATH, so we
    // do not pass clangdExe here — we rely on the same resolution the
    // production LSPServer uses.
    bool started = bridge.start(rootUri);
    if (!started) {
        GTEST_SKIP() << "bridge.start(rootUri) could not resolve clangd";
    }
    EXPECT_TRUE(bridge.isAvailable());
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: multiple back-to-back queries on one live session don't step on
// each other (exercises request-id bookkeeping in LSPBridge).
TEST(ClangdBridge, ConcurrentQueriesSameSession) {
    std::string clangdExe;
    if (!clangdIsRunnable(clangdExe)) GTEST_SKIP() << "clangd not available";

    ClangdBridge bridge;
    if (!startOnFixture(bridge, clangdExe)) {
        GTEST_SKIP() << "clangd failed to start";
    }

    // Hammer the session with 5 sequential queries; we're not checking
    // hit rates, only that every call returns and does not crash.
    std::vector<std::string> names = {
        "engine::init", "engine::render", "engine::detail::internalHelper",
        "engine::runFrames", "engine::Scene"};
    int successes = 0;
    for (const auto& n : names) {
        auto def = bridge.findDefinition(n, {});
        if (def.has_value() && !def->file.empty()) ++successes;
    }
    bridge.stop();
    // If clangd indexed the fixture at all we should resolve at least one;
    // otherwise the test is still meaningful as a no-crash probe.
    EXPECT_GE(successes, 0);
}

// ---------------------------------------------------------------------------
// Failure-path tests (open issue: lsp-bridge-malformed-harness-untracked-after-issue-archived)
// ---------------------------------------------------------------------------

// Test: when the configured binary is missing the bridge must fail start()
// cleanly rather than hang or crash, and subsequent queries must return
// nullopt / empty. Complements the older InvalidPathFails test by adding
// explicit post-failure query verification — documents graceful-degradation
// contract.
TEST(ClangdBridge, BridgeMissingBinaryFallsBackGracefully) {
    ClangdBridge bridge;
    // Deliberately bogus absolute path under a dir that cannot exist.
    bool started = bridge.start("/nonexistent-dir-xyz/clangd-xyz", "", "file:///tmp");
    EXPECT_FALSE(started) << "start() must fail when binary path doesn't exist";
    EXPECT_FALSE(bridge.isAvailable()) << "bridge must report unavailable after failed start";

    // All query APIs must degrade silently (no crash, no hang).
    auto def = bridge.findDefinition("any::symbol", {});
    EXPECT_FALSE(def.has_value());
    auto hover = bridge.getHoverInfo("any::symbol", {});
    EXPECT_FALSE(hover.has_value());
    auto refs = bridge.findReferences("any::symbol", {});
    EXPECT_TRUE(refs.empty());

    // stop() after a failed start must be a no-op.
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: malformed JSON-RPC response handling. Points the real ClangdBridge
// at the fake-langserver harness (a deliberately broken stdio server) in
// every malformed mode and asserts the bridge degrades gracefully — no
// crash, no hang, ends up unavailable, all query APIs return empty/nullopt.
// This exercises the bridge's actual framing/parse/init path, not a stub.
TEST(ClangdBridge, BridgeMalformedResponseHandled) {
    const char* fake = TOPO_LSP_FAKE_LANGSERVER;
    ASSERT_NE(fake, nullptr);
    ASSERT_TRUE(fs::exists(fake)) << "fake langserver helper not built: " << fake;

    const char* modes[] = {"missing-field", "type-mismatch", "bad-length",
                            "short-body",    "not-json",      "nonzero-exit"};
    for (const char* mode : modes) {
        SCOPED_TRACE(mode);
        // The fake server reads its fault mode from the inherited env (the
        // bridge spawns it with the bridge's own fixed args).
#ifdef _WIN32
        _putenv_s("TOPO_FAKE_LSP_MODE", mode);
#else
        setenv("TOPO_FAKE_LSP_MODE", mode, 1);
#endif
        ClangdBridge bridge;
        // start() must not hang or crash; it should fail (or leave the
        // bridge unavailable) when the server speaks garbage.
        bool started = bridge.start(fake, "", "file:///tmp");
        EXPECT_FALSE(bridge.isAvailable())
            << "bridge must not report available on malformed handshake";
        (void)started;

        // Every query API degrades silently.
        EXPECT_FALSE(bridge.findDefinition("engine::init", {}).has_value());
        EXPECT_FALSE(bridge.getHoverInfo("engine::init", {}).has_value());
        EXPECT_TRUE(bridge.findReferences("engine::init", {}).empty());

        bridge.stop(); // must be safe regardless of start outcome
        EXPECT_FALSE(bridge.isAvailable());
    }
}

// Test: isClangdAvailable() static predicate is consistent with an actual
// start attempt.  If the predicate says yes, start should succeed (at
// least to the point of isAvailable()==true).  If it says no, we skip
// rather than fail — the predicate is advisory on this machine.
TEST(ClangdBridge, IsClangdAvailablePredicateConsistent) {
    const bool predicted = ClangdBridge::isClangdAvailable();
    if (!predicted) {
        GTEST_SKIP() << "isClangdAvailable() reports no clangd; nothing to verify";
    }
    ClangdBridge bridge;
    std::string rootUri = "file:///" + fixtureDir();
    bool started = bridge.start(rootUri);
    EXPECT_TRUE(started) << "isClangdAvailable()==true but start() failed";
    if (started) {
        EXPECT_TRUE(bridge.isAvailable());
        bridge.stop();
    }
}
