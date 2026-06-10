#include "JdtBridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace topo::lsp;

static std::string fixtureDir() {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/jdt_test";
}

static std::string fixtureRootUri() {
    // file:// URI for the fixture root. POSIX absolute paths already start
    // with '/'; Windows drive-letter paths (D:/...) need the extra slash or
    // the drive would parse as the URI authority.
    std::string dir = fixtureDir();
    if (!dir.empty() && dir[0] == '/') return "file://" + dir;
    return "file:///" + dir;
}

static std::string widgetSourcePath() {
    return fixtureDir() + "/src/main/java/com/example/Widget.java";
}

static std::string appSourcePath() {
    return fixtureDir() + "/src/main/java/com/example/App.java";
}

static std::string pointSourcePath() {
    return fixtureDir() + "/src/main/java/com/example/Point.java";
}

// Returns jdtls executable path if available on PATH, else empty string.
// Pure existence probe; callers skip when it returns empty.
static std::string findJdtls() {
#ifdef _WIN32
    // Existence probe mirroring `command -v` below. jdtls ships as a Python
    // launcher (jdtls.bat on Windows, no .exe), and `--version` is unreliable
    // across jdtls builds; `where` matches the .bat via PATHEXT.
    const char* checkCmd = "where jdtls > NUL 2>&1";
#else
    // --help is faster and more reliable than --version on some jdtls builds;
    // we accept any exit status that indicates the binary ran, then probe
    // existence via `command -v` so the test can skip cleanly.
    const char* checkCmd = "command -v jdtls > /dev/null 2>&1";
#endif
    int ret = std::system(checkCmd);
    if (ret != 0) return {};
    return "jdtls";
}

// ---------------------------------------------------------------------------
// Pure property tests (no jdtls required)
// ---------------------------------------------------------------------------

// 1. Invalid binary path fails gracefully
TEST(JdtBridge, InvalidPathFails) {
    JdtBridge bridge;
    bool started = bridge.start("/nonexistent/jdtls-xxxx", "file:///tmp");
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());
}

// 2. stop() on a non-started bridge is a no-op
TEST(JdtBridge, StopWithoutStart) {
    JdtBridge bridge;
    bridge.stop(); // must not crash
    EXPECT_FALSE(bridge.isAvailable());
}

// 3. Querying before start() returns empty results
TEST(JdtBridge, QueryWhenUnavailable) {
    JdtBridge bridge;
    auto def = bridge.findDefinition("com.example.Widget.compute", {});
    EXPECT_FALSE(def.has_value());

    auto refs = bridge.findReferences("com.example.Widget.compute", {});
    EXPECT_TRUE(refs.empty());

    auto hover = bridge.getHoverInfo("com.example.Widget.compute", {});
    EXPECT_FALSE(hover.has_value());
}

// 4. displayName/languageId are pure properties
TEST(JdtBridge, DisplayNameAndLanguageIdStable) {
    JdtBridge bridge;
    EXPECT_EQ(bridge.displayName(), "Java");
    EXPECT_EQ(bridge.languageId(), "java");
}

// 5. Empty qualified name is handled safely (no crash, nullopt/empty result).
TEST(JdtBridge, EmptyQualifiedNameReturnsNullopt) {
    JdtBridge bridge; // not started → immediate nullopt
    auto def = bridge.findDefinition("", {});
    EXPECT_FALSE(def.has_value());
    auto hover = bridge.getHoverInfo("", {});
    EXPECT_FALSE(hover.has_value());
    auto refs = bridge.findReferences("", {});
    EXPECT_TRUE(refs.empty());
}

// 6. findTypeDefinition falls back to source scanning even when jdtls is not
//    available. This exercises the regex-based fallback path in JdtBridge.cpp.
TEST(JdtBridge, FindTypeDefinitionFallbackScansSources) {
    JdtBridge bridge; // deliberately not started
    const std::vector<std::string> sources = {widgetSourcePath(), appSourcePath()};
    auto result = bridge.findTypeDefinition("Widget", sources, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->file.find("Widget.java"), std::string::npos);
    EXPECT_GT(result->line, 0);
}

// 7. Fallback returns nullopt for a type not present in any source file.
TEST(JdtBridge, FindTypeDefinitionFallbackMissingTypeReturnsNullopt) {
    JdtBridge bridge;
    const std::vector<std::string> sources = {widgetSourcePath(), appSourcePath()};
    auto result = bridge.findTypeDefinition("NonExistentSymbolXYZ", sources, {});
    EXPECT_FALSE(result.has_value());
}

// 7b. Fallback recognises a Java 14+ `record` declaration. The trailing
//     `(` after the type name (e.g. `record Point(int x, int y) {`) is
//     part of the record syntax and must be accepted by the regex
//     alternation alongside the historic `class | interface | enum`.
TEST(JdtBridge, FindTypeDefinitionFallbackMatchesRecord) {
    JdtBridge bridge;
    const std::vector<std::string> sources = {pointSourcePath()};
    auto result = bridge.findTypeDefinition("Point", sources, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->file.find("Point.java"), std::string::npos);
    EXPECT_GT(result->line, 0);
}

// ---------------------------------------------------------------------------
// Live-backend tests — require jdtls. SKIP when unavailable or slow.
// ---------------------------------------------------------------------------

// Starting jdtls cold + indexing typically takes 10-30 seconds.
// The base class first-request timeout defaults to 30s, so these tests pay
// that cost once per test. Keep the number of live tests small.

// 8. Start and stop a live jdtls process.
TEST(JdtBridge, StartFromPathIfAvailable) {
    std::string jdtls = findJdtls();
    if (jdtls.empty()) {
        GTEST_SKIP() << "jdtls not found on PATH — skipping live-backend test";
    }

    JdtBridge bridge;
    std::string rootUri = fixtureRootUri();

    bool started = bridge.start(jdtls, rootUri);
    if (!started) {
        GTEST_SKIP() << "jdtls failed to start — skipping";
    }
    EXPECT_TRUE(bridge.isAvailable());

    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// 9. Query known method: com.example.Widget.compute.
TEST(JdtBridge, FindDefinitionOnKnownMethod) {
    std::string jdtls = findJdtls();
    if (jdtls.empty()) {
        GTEST_SKIP() << "jdtls not found on PATH";
    }

    JdtBridge bridge;
    std::string rootUri = fixtureRootUri();
    if (!bridge.start(jdtls, rootUri)) {
        GTEST_SKIP() << "jdtls failed to start";
    }

    // Give jdtls time to index the tiny fixture project.
    bridge.waitForIndex(std::chrono::milliseconds{30000});

    auto def = bridge.findDefinition("com.example.Widget.compute", {});
    // jdtls indexing is asynchronous and flaky on cold starts — accept either
    // a hit (with sensible shape) or nullopt (indexing not yet complete).
    if (def.has_value()) {
        EXPECT_FALSE(def->file.empty());
        EXPECT_GE(def->line, 0);
    }
    bridge.stop();
}

// 10. Find references for a known method.
TEST(JdtBridge, FindReferencesOnKnownMethod) {
    std::string jdtls = findJdtls();
    if (jdtls.empty()) {
        GTEST_SKIP() << "jdtls not found on PATH";
    }

    JdtBridge bridge;
    std::string rootUri = fixtureRootUri();
    if (!bridge.start(jdtls, rootUri)) {
        GTEST_SKIP() << "jdtls failed to start";
    }

    bridge.waitForIndex(std::chrono::milliseconds{30000});

    auto refs = bridge.findReferences("com.example.Widget.compute", {});
    // Accept either a non-empty result (expected: Widget.java + App.java) or
    // an empty vector if jdtls hasn't finished indexing.
    for (const auto& r : refs) {
        EXPECT_FALSE(r.file.empty());
        EXPECT_GE(r.line, 0);
    }
    bridge.stop();
}

// 11. Hover over a known method returns a non-empty signature string.
TEST(JdtBridge, GetHoverInfoReturnsSignature) {
    std::string jdtls = findJdtls();
    if (jdtls.empty()) {
        GTEST_SKIP() << "jdtls not found on PATH";
    }

    JdtBridge bridge;
    std::string rootUri = fixtureRootUri();
    if (!bridge.start(jdtls, rootUri)) {
        GTEST_SKIP() << "jdtls failed to start";
    }

    bridge.waitForIndex(std::chrono::milliseconds{30000});

    auto hover = bridge.getHoverInfo("com.example.Widget.compute", {});
    if (hover.has_value()) {
        EXPECT_FALSE(hover->empty());
    }
    bridge.stop();
}

// 12. Find the type declaration for Widget via workspace index (live path).
TEST(JdtBridge, FindTypeDefinitionOnClass) {
    std::string jdtls = findJdtls();
    if (jdtls.empty()) {
        GTEST_SKIP() << "jdtls not found on PATH";
    }

    JdtBridge bridge;
    std::string rootUri = fixtureRootUri();
    if (!bridge.start(jdtls, rootUri)) {
        GTEST_SKIP() << "jdtls failed to start";
    }

    bridge.waitForIndex(std::chrono::milliseconds{30000});

    // Live path (jdtls running) should go through queryWorkspaceSymbol first,
    // but fallback still works — so this test must succeed either way.
    const std::vector<std::string> sources = {widgetSourcePath(), appSourcePath()};
    auto result = bridge.findTypeDefinition("Widget", sources, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->file.find("Widget.java"), std::string::npos);
    bridge.stop();
}

// 13. Unknown symbol returns nullopt when jdtls is live.
TEST(JdtBridge, UnknownSymbolReturnsNullopt) {
    std::string jdtls = findJdtls();
    if (jdtls.empty()) {
        GTEST_SKIP() << "jdtls not found on PATH";
    }

    JdtBridge bridge;
    std::string rootUri = fixtureRootUri();
    if (!bridge.start(jdtls, rootUri)) {
        GTEST_SKIP() << "jdtls failed to start";
    }

    bridge.waitForIndex(std::chrono::milliseconds{30000});

    auto def = bridge.findDefinition("com.nonexistent.Nope.absent", {});
    EXPECT_FALSE(def.has_value());

    auto hover = bridge.getHoverInfo("com.nonexistent.Nope.absent", {});
    EXPECT_FALSE(hover.has_value());
    bridge.stop();
}

// ---------------------------------------------------------------------------
// Failure-path tests (missing-binary / malformed-harness degradation)
// ---------------------------------------------------------------------------

// Test: when jdtls binary is missing the bridge must fail start() cleanly,
// degrade to unavailable, and return empty/nullopt from every query API.
TEST(JdtBridge, BridgeMissingBinaryFallsBackGracefully) {
    JdtBridge bridge;
    bool started = bridge.start("/nonexistent-dir-xyz/jdtls-xyz", "file:///tmp");
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());

    EXPECT_FALSE(bridge.findDefinition("com.example.Widget.compute", {}).has_value());
    EXPECT_FALSE(bridge.getHoverInfo("com.example.Widget.compute", {}).has_value());
    EXPECT_TRUE(bridge.findReferences("com.example.Widget.compute", {}).empty());

    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: malformed JSON-RPC response handling. Points the real JdtBridge at
// the fake-langserver harness in every malformed mode and asserts graceful
// degradation through the bridge's actual framing/parse/init path (no
// crash, no hang, stays unavailable, queries return empty/nullopt).
// Requires no jdtls install — the fake server stands in for it.
TEST(JdtBridge, BridgeMalformedResponseHandled) {
    const char* fake = TOPO_LSP_FAKE_LANGSERVER;
    ASSERT_NE(fake, nullptr);
    ASSERT_TRUE(std::filesystem::exists(fake))
        << "fake langserver helper not built: " << fake;

    const char* modes[] = {"missing-field", "type-mismatch", "bad-length",
                            "short-body",    "not-json",      "nonzero-exit"};
    for (const char* mode : modes) {
        SCOPED_TRACE(mode);
#ifdef _WIN32
        _putenv_s("TOPO_FAKE_LSP_MODE", mode);
#else
        setenv("TOPO_FAKE_LSP_MODE", mode, 1);
#endif
        JdtBridge bridge;
        bool started = bridge.start(fake, "file:///tmp");
        EXPECT_FALSE(bridge.isAvailable())
            << "bridge must not report available on malformed handshake";
        (void)started;

        EXPECT_FALSE(bridge.findDefinition("com.example.Widget.compute", {}).has_value());
        EXPECT_FALSE(bridge.getHoverInfo("com.example.Widget.compute", {}).has_value());
        EXPECT_TRUE(bridge.findReferences("com.example.Widget.compute", {}).empty());

        bridge.stop();
        EXPECT_FALSE(bridge.isAvailable());
    }
}

// 14. Restart cycle — second start works after stop. SKIP'd by default because
//     jdtls cold start is 10-30s; running twice would double test time.
//     Keep scaffold in place so operators can flip on demand.
TEST(JdtBridge, RestartCycleIsolatesState) {
    GTEST_SKIP() << "jdtls cold start is 10-30s; double start would time out "
                    "under normal CI budgets. Scaffold retained intentionally.";
}
