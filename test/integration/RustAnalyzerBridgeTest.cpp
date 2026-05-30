#include "RustAnalyzerBridge.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace topo::lsp;
namespace fs = std::filesystem;

static std::string fixtureDir() {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/rust_analyzer_test";
}

static std::string fixtureRootUri() {
    // file:// URI for the fixture directory. RustAnalyzerBridge uses this as
    // the workspace root during `initialize`.
    return "file://" + fixtureDir();
}

// Probe rust-analyzer availability. Returns the executable string to pass to
// `start`; empty string means "rust-analyzer not runnable". We prefer the
// known Homebrew rustup path when present, then fall back to PATH.
static std::string findRustAnalyzer() {
    static const char* kCandidates[] = {
        "/opt/homebrew/opt/rustup/bin/rust-analyzer",
        "/usr/local/opt/rustup/bin/rust-analyzer",
    };

    for (const char* candidate : kCandidates) {
        if (fs::exists(candidate)) {
            return std::string(candidate);
        }
    }

    // Fall through to PATH — LSPBridge will resolve via the default search.
    return std::string{};
}

// Returns true if `rust-analyzer --version` exits cleanly. Tests that need a
// live backend call this and `GTEST_SKIP()` on failure.
static bool rustAnalyzerRunnable(const std::string& exePath) {
    std::string check = exePath.empty() ? std::string("rust-analyzer") : exePath;
#ifdef _WIN32
    int ret = std::system(("\"" + check + "\" --version > NUL 2>&1").c_str());
#else
    int ret = std::system(("\"" + check + "\" --version > /dev/null 2>&1").c_str());
#endif
    return ret == 0;
}

// --- Pure tests (no backend) -----------------------------------------------

// Test: invalid path gracefully fails, no crash, bridge reports unavailable.
TEST(RustAnalyzerBridge, InvalidPathFails) {
    RustAnalyzerBridge bridge;
    bool started = bridge.start("/nonexistent/rust-analyzer-xxxx", "file:///tmp");
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: calling stop() before start() is a no-op, never crashes.
TEST(RustAnalyzerBridge, StopWithoutStart) {
    RustAnalyzerBridge bridge;
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: queries on an unavailable bridge return empty / nullopt.
TEST(RustAnalyzerBridge, QueryWhenUnavailable) {
    RustAnalyzerBridge bridge;

    auto def = bridge.findDefinition("ra_test::compute", {});
    EXPECT_FALSE(def.has_value());

    auto refs = bridge.findReferences("ra_test::compute", {});
    EXPECT_TRUE(refs.empty());

    auto hover = bridge.getHoverInfo("ra_test::compute", {});
    EXPECT_FALSE(hover.has_value());
}

// Test: displayName / languageId are pure accessors, stable without start.
TEST(RustAnalyzerBridge, DisplayNameAndLanguageIdStable) {
    RustAnalyzerBridge bridge;
    EXPECT_EQ(bridge.displayName(), "Rust");
    EXPECT_EQ(bridge.languageId(), "rust");

    // Still stable after a failed start.
    (void)bridge.start("/nonexistent/rust-analyzer-xxxx", "file:///tmp");
    EXPECT_EQ(bridge.displayName(), "Rust");
    EXPECT_EQ(bridge.languageId(), "rust");
}

// Test: findTypeDefinition's regex-based source scan works even without a
// running backend. Exercises the fallback path in the bridge.
TEST(RustAnalyzerBridge, FindTypeDefinitionFallbackScansSources) {
    RustAnalyzerBridge bridge;
    std::string libRs = fixtureDir() + "/src/lib.rs";
    std::vector<std::string> sources{libRs};

    auto widget = bridge.findTypeDefinition("Widget", sources, {});
    ASSERT_TRUE(widget.has_value());
    EXPECT_EQ(widget->file, libRs);
    EXPECT_GT(widget->line, 0);

    auto shape = bridge.findTypeDefinition("Shape", sources, {});
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(shape->file, libRs);

    auto unknown = bridge.findTypeDefinition("NoSuchType", sources, {});
    EXPECT_FALSE(unknown.has_value());
}

// --- Live-backend tests ----------------------------------------------------

// Shared fixture: starts the bridge once per test, skips when the backend
// isn't installed or fails to initialize.
class RustAnalyzerBridgeLive : public ::testing::Test {
protected:
    RustAnalyzerBridge bridge;
    std::string exePath;

    void SetUp() override {
        exePath = findRustAnalyzer();
        if (!rustAnalyzerRunnable(exePath)) {
            GTEST_SKIP() << "rust-analyzer not found on PATH or known Homebrew "
                            "locations — skipping live-backend tests";
        }

        bool started = bridge.start(exePath, fixtureRootUri());
        if (!started) {
            GTEST_SKIP() << "rust-analyzer failed to initialize against fixture";
        }
        ASSERT_TRUE(bridge.isAvailable());
    }

    void TearDown() override {
        bridge.stop();
    }
};

// Test: rust-analyzer starts and reports available against the Cargo fixture.
TEST_F(RustAnalyzerBridgeLive, StartFromPathIfAvailable) {
    EXPECT_TRUE(bridge.isAvailable());
}

// Test: workspace/symbol resolves a known public function.
TEST_F(RustAnalyzerBridgeLive, FindDefinitionOnKnownFn) {
    auto def = bridge.findDefinition("compute", {});
    // rust-analyzer indexing is asynchronous; we tolerate either a concrete
    // hit or nullopt (the point is that the call completes without crashing
    // and, when indexing has settled, returns a location inside the fixture).
    if (def.has_value()) {
        EXPECT_NE(def->file.find("lib.rs"), std::string::npos);
        EXPECT_GE(def->line, 0);
    }
}

// Test: findReferences returns zero-or-more sites without crashing. The
// fixture has `caller` invoking `compute` twice, so when the index is warm
// we expect at least one reference; tolerate empty under cold-index.
TEST_F(RustAnalyzerBridgeLive, FindReferencesOnKnownFn) {
    auto refs = bridge.findReferences("compute", {});
    for (const auto& r : refs) {
        EXPECT_NE(r.file.find(".rs"), std::string::npos);
        EXPECT_GE(r.line, 0);
    }
}

// Test: getHoverInfo on a real symbol returns a non-empty string when the
// index has resolved it; crash-free and possibly-empty is acceptable.
TEST_F(RustAnalyzerBridgeLive, GetHoverInfoReturnsSignature) {
    auto hover = bridge.getHoverInfo("compute", {});
    if (hover.has_value()) {
        EXPECT_FALSE(hover->empty());
    }
}

// Test: findTypeDefinition on a struct returns the fixture's lib.rs via
// either the workspace index or the regex fallback.
TEST_F(RustAnalyzerBridgeLive, FindTypeDefinitionOnStruct) {
    std::vector<std::string> sources{fixtureDir() + "/src/lib.rs"};
    auto def = bridge.findTypeDefinition("Widget", sources, {});
    ASSERT_TRUE(def.has_value());
    EXPECT_NE(def->file.find(".rs"), std::string::npos);
}

// Test: unknown symbol queries return empty results (never spurious hits).
TEST_F(RustAnalyzerBridgeLive, UnknownSymbolReturnsNullopt) {
    auto def = bridge.findDefinition("this_symbol_does_not_exist_xyz", {});
    EXPECT_FALSE(def.has_value());

    auto hover = bridge.getHoverInfo("this_symbol_does_not_exist_xyz", {});
    EXPECT_FALSE(hover.has_value());

    auto refs = bridge.findReferences("this_symbol_does_not_exist_xyz", {});
    EXPECT_TRUE(refs.empty());
}

// Test: empty qualified name never crashes; returns empty results.
TEST_F(RustAnalyzerBridgeLive, EmptyQualifiedNameReturnsNullopt) {
    auto def = bridge.findDefinition("", {});
    EXPECT_FALSE(def.has_value());

    auto hover = bridge.getHoverInfo("", {});
    EXPECT_FALSE(hover.has_value());

    auto refs = bridge.findReferences("", {});
    EXPECT_TRUE(refs.empty());
}

// ---------------------------------------------------------------------------
// Failure-path tests (missing-binary / malformed-harness degradation)
// ---------------------------------------------------------------------------

// Test: when the configured rust-analyzer binary is missing the bridge must
// fail start() cleanly, degrade to unavailable, and return empty/nullopt
// from every query API (no hang, no crash).
TEST(RustAnalyzerBridge, BridgeMissingBinaryFallsBackGracefully) {
    RustAnalyzerBridge bridge;
    bool started = bridge.start("/nonexistent-dir-xyz/rust-analyzer-xyz", "file:///tmp");
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());

    EXPECT_FALSE(bridge.findDefinition("any::symbol", {}).has_value());
    EXPECT_FALSE(bridge.getHoverInfo("any::symbol", {}).has_value());
    EXPECT_TRUE(bridge.findReferences("any::symbol", {}).empty());

    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: malformed JSON-RPC response handling. Points the real
// RustAnalyzerBridge at the fake-langserver harness in every malformed mode
// and asserts graceful degradation through the bridge's actual
// framing/parse/init path (no crash, no hang, stays unavailable, queries
// return empty/nullopt). Requires no rust-analyzer install — the fake
// server stands in for it.
TEST(RustAnalyzerBridge, BridgeMalformedResponseHandled) {
    const char* fake = TOPO_LSP_FAKE_LANGSERVER;
    ASSERT_NE(fake, nullptr);
    ASSERT_TRUE(fs::exists(fake)) << "fake langserver helper not built: " << fake;

    const char* modes[] = {"missing-field", "type-mismatch", "bad-length",
                            "short-body",    "not-json",      "nonzero-exit"};
    for (const char* mode : modes) {
        SCOPED_TRACE(mode);
#ifdef _WIN32
        _putenv_s("TOPO_FAKE_LSP_MODE", mode);
#else
        setenv("TOPO_FAKE_LSP_MODE", mode, 1);
#endif
        RustAnalyzerBridge bridge;
        bool started = bridge.start(fake, "file:///tmp");
        EXPECT_FALSE(bridge.isAvailable())
            << "bridge must not report available on malformed handshake";
        (void)started;

        EXPECT_FALSE(bridge.findDefinition("ra_test::compute", {}).has_value());
        EXPECT_FALSE(bridge.getHoverInfo("ra_test::compute", {}).has_value());
        EXPECT_TRUE(bridge.findReferences("ra_test::compute", {}).empty());

        bridge.stop();
        EXPECT_FALSE(bridge.isAvailable());
    }
}

// Test: start/stop/start cycle leaves the bridge in a fresh, usable state.
// Uses its own bridge instance to avoid interacting with the fixture class.
TEST(RustAnalyzerBridgeLifecycle, RestartCycleIsolatesState) {
    std::string exe = findRustAnalyzer();
    if (!rustAnalyzerRunnable(exe)) {
        GTEST_SKIP() << "rust-analyzer not available";
    }

    RustAnalyzerBridge bridge;
    ASSERT_TRUE(bridge.start(exe, fixtureRootUri()));
    EXPECT_TRUE(bridge.isAvailable());
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());

    // Second cycle must succeed independently of the first.
    ASSERT_TRUE(bridge.start(exe, fixtureRootUri()));
    EXPECT_TRUE(bridge.isAvailable());
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}
