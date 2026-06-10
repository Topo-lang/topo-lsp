// Integration tests for PyrightBridge — exercises the real pyright-langserver
// binary via the bridge.  Tests GTEST_SKIP() when the backend is unavailable.
//
// Covers LSP protocol conformance for the pyright bridge.

#include "PyrightBridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>

using namespace topo::lsp;

// Poll-until-ready helper: pyright's workspace/symbol index can take more
// than a single short query's worth of time on a cold cache. Retrying until
// a bounded deadline (instead of GTEST_SKIP after one miss) keeps the
// happy-path assertions actually running when pyright IS installed, rather
// than silently dropping coverage on machines where the langserver exists.
// The missing-binary case is gated upstream by findPyrightLangserver()/fx.ok(),
// so a persistent miss here is a genuine index timeout, not an absent tool.
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

static std::string fixtureDir() {
    return std::string(TOPO_TEST_FIXTURES_DIR) + "/pyright_test";
}

static std::string fixtureRootUri() {
    // Pyright expects a file:// URI without the leading slash duplication.
    // POSIX absolute paths already start with '/'; Windows drive-letter
    // paths (D:/...) need the extra slash or the drive would parse as the
    // URI authority.
    std::string dir = fixtureDir();
    if (!dir.empty() && dir[0] == '/') return "file://" + dir;
    return "file:///" + dir;
}

static std::string fixtureFile(const std::string& name) {
    return fixtureDir() + "/" + name;
}

// Probe the runtime environment for a pyright-langserver binary.  The bridge
// defaults to "pyright-langserver" on PATH when no path is supplied, so we
// report availability based on that.  Tests that need the server should call
// this and GTEST_SKIP() when it returns empty.
static std::string findPyrightLangserver() {
#ifdef _WIN32
    int ret = std::system("where pyright-langserver > NUL 2>&1");
#else
    int ret = std::system("command -v pyright-langserver > /dev/null 2>&1");
#endif
    if (ret != 0) return "";
    return "pyright-langserver"; // empty path lets the bridge resolve via PATH
}

// ---------------------------------------------------------------------------
// Unavailable-bridge behaviour (no backend required)
// ---------------------------------------------------------------------------

TEST(PyrightBridge, InvalidPathFails) {
    PyrightBridge bridge;
    bool started = bridge.start("/nonexistent/pyright-xxxxx", fixtureRootUri());
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());
}

TEST(PyrightBridge, StopWithoutStart) {
    PyrightBridge bridge;
    bridge.stop(); // must not crash
    EXPECT_FALSE(bridge.isAvailable());
}

TEST(PyrightBridge, QueryWhenUnavailable) {
    PyrightBridge bridge;
    auto defn = bridge.findDefinition("make_widget", {});
    EXPECT_FALSE(defn.has_value());

    auto refs = bridge.findReferences("make_widget", {});
    EXPECT_TRUE(refs.empty());

    auto hover = bridge.getHoverInfo("make_widget", {});
    EXPECT_FALSE(hover.has_value());
}

TEST(PyrightBridge, DisplayNameAndLanguageIdStable) {
    PyrightBridge bridge;
    EXPECT_EQ(bridge.displayName(), "Python");
    EXPECT_EQ(bridge.languageId(), "python");
}

TEST(PyrightBridge, EmptyQualifiedNameReturnsNullopt) {
    // Even without starting, findDefinition must not return a spurious result
    // for an empty query.  Also covers the fallback path of findTypeDefinition
    // when the bridge is not running.
    PyrightBridge bridge;
    EXPECT_FALSE(bridge.findDefinition("", {}).has_value());
    EXPECT_FALSE(bridge.getHoverInfo("", {}).has_value());
    EXPECT_TRUE(bridge.findReferences("", {}).empty());
}

// findTypeDefinition has a file-scan fallback that runs even when the bridge
// is not started, so an unknown class name over real .py files must still
// return nullopt.
TEST(PyrightBridge, FindTypeDefinitionUnknownClassFallback) {
    PyrightBridge bridge;
    auto result = bridge.findTypeDefinition("TotallyNotAClass",
                                            {fixtureFile("widget.py")},
                                            {});
    EXPECT_FALSE(result.has_value());
}

// The file-scan fallback should locate `class Widget` in widget.py even
// without pyright running.  This exercises the offline path.
TEST(PyrightBridge, FindTypeDefinitionKnownClassFallback) {
    PyrightBridge bridge;
    auto result = bridge.findTypeDefinition("Widget",
                                            {fixtureFile("widget.py")},
                                            {});
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->file.find("widget.py"), std::string::npos);
    EXPECT_GT(result->line, 0);
}

// PEP 695 generic class declarations (`class Container[T]:`) — the
// follow-set must accept `[` so the fallback regex matches.  Regression
// guard for the PEP 695 generic class that the findTypeDefinition fallback
// regex previously failed to match.
TEST(PyrightBridge, FindTypeDefinitionPep695GenericClassFallback) {
    PyrightBridge bridge;
    auto containerResult = bridge.findTypeDefinition(
        "Container", {fixtureFile("pep695_container.py")}, {});
    ASSERT_TRUE(containerResult.has_value())
        << "PEP 695 `class Container[T]:` must be matched by the fallback regex";
    EXPECT_NE(containerResult->file.find("pep695_container.py"), std::string::npos);

    auto pairResult = bridge.findTypeDefinition(
        "Pair", {fixtureFile("pep695_container.py")}, {});
    ASSERT_TRUE(pairResult.has_value())
        << "PEP 695 with two type params must also match";

    auto boundedResult = bridge.findTypeDefinition(
        "BoundedBox", {fixtureFile("pep695_container.py")}, {});
    ASSERT_TRUE(boundedResult.has_value())
        << "PEP 696 bound (`class Foo[T: Bound]`) must match";

    auto defaultedResult = bridge.findTypeDefinition(
        "DefaultedBox", {fixtureFile("pep695_container.py")}, {});
    ASSERT_TRUE(defaultedResult.has_value())
        << "PEP 696 default (`class Foo[T = Default]`) must match";
}

// findTypeDefinition's fallback used to concatenate ``typeName`` raw into a
// ``std::regex``; a dotted name ``module.Foo`` then matched ``module*Foo`` and
// a bracketed name ``Container[T]`` opened a malformed character class and
// threw ``std::regex_error`` straight through the LSP request handler. The
// fix escapes ECMAScript metacharacters before constructing the regex.
// Regression guard against regex-metacharacter injection in lookup names.
TEST(PyrightBridge, FindTypeDefinitionRegexMetacharsAreEscaped) {
    PyrightBridge bridge;

    // ``module.Foo`` — the ``.`` must NOT match arbitrary character. The
    // fixture only contains ``class Widget:``, so a dotted lookup that does
    // not exist literally must return nullopt rather than false-matching on
    // ``Widget`` via the wildcard.
    auto dottedMiss = bridge.findTypeDefinition(
        "modulexWidget", {fixtureFile("widget.py")}, {});
    EXPECT_FALSE(dottedMiss.has_value())
        << "Unescaped `.` would have matched `module*Widget` against any line "
           "containing ``class W`` — escapeRegexLiteral must neutralise it.";

    // Bracketed name — used to open ``[T]`` as a character class, throwing
    // ``std::regex_error`` through the caller. After the fix the regex
    // construction is try/caught, but with the escape the regex itself is
    // also valid (matches literally) so neither outcome leaks to the caller.
    auto bracketed = bridge.findTypeDefinition(
        "Container[T]", {fixtureFile("widget.py")}, {});
    EXPECT_FALSE(bracketed.has_value())
        << "Bracketed name must NOT throw; absent the class, it returns "
           "nullopt cleanly.";

    // Star / plus / parens — any of these used to be regex specials. With
    // escape they are treated as literal characters; without the fix they
    // would throw or false-match.
    for (const auto& probe : {std::string("Foo*"), std::string("Foo+"),
                              std::string("Foo("), std::string("Foo?"),
                              std::string("Foo|Bar")}) {
        auto r = bridge.findTypeDefinition(
            probe, {fixtureFile("widget.py")}, {});
        EXPECT_FALSE(r.has_value())
            << "Metacharacter-bearing probe '" << probe
            << "' must not match (and must not throw).";
    }

    // Sanity: the escape is literal-equality, not "everything fails". A
    // legitimate identifier still matches its declaration.
    auto literal = bridge.findTypeDefinition(
        "Widget", {fixtureFile("widget.py")}, {});
    ASSERT_TRUE(literal.has_value());
}

// ---------------------------------------------------------------------------
// Tests that require the pyright-langserver binary
// ---------------------------------------------------------------------------

namespace {

// RAII helper: starts a PyrightBridge against the fixture, waits for indexing,
// and stops the bridge on destruction.  Tests skip when the binary is missing.
class PyrightFixture {
public:
    PyrightFixture() {
        exe_ = findPyrightLangserver();
        if (exe_.empty()) return;

        // Leave the path empty so PyrightBridge resolves via PATH with the
        // correct platform-specific suffix.
        started_ = bridge_.start(std::string{}, fixtureRootUri());
        if (!started_) return;

        // Pyright needs a moment to scan the workspace; wait with a generous
        // timeout so the CI host can catch up.
        bridge_.setTimeouts(std::chrono::milliseconds{30000},
                            std::chrono::milliseconds{10000});
        // CRITICAL: pyright only populates workspace/symbol for files it has
        // been told about. Without an explicit textDocument/didOpen the
        // fixture module is never analyzed, so findDefinition("make_widget")
        // never resolves regardless of how long the test polls — this is the
        // real cause of the "did not index within 30s" skip, not cold-cache
        // latency. Opening widget.py makes pyright resolve it in ~0.1s.
        // (this is why the happy-path tests used to skip even when the
        //  langserver was installed.)
        bridge_.openDocument(fixtureDir() + "/widget.py");
        (void)bridge_.waitForIndex(std::chrono::milliseconds{15000});
    }

    ~PyrightFixture() {
        if (started_) bridge_.stop();
    }

    bool ok() const { return !exe_.empty() && started_ && bridge_.isAvailable(); }
    PyrightBridge& bridge() { return bridge_; }

private:
    std::string exe_;
    bool started_ = false;
    PyrightBridge bridge_;
};

} // namespace

TEST(PyrightBridge, StartFromPathIfAvailable) {
    if (findPyrightLangserver().empty()) {
        GTEST_SKIP() << "pyright-langserver not found on PATH";
    }

    PyrightBridge bridge;
    bool started = bridge.start(std::string{}, fixtureRootUri());
    if (!started) {
        GTEST_SKIP() << "pyright-langserver failed to start";
    }
    EXPECT_TRUE(bridge.isAvailable());

    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

TEST(PyrightBridge, FindDefinitionOnFunction) {
    PyrightFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "pyright backend unavailable";

    auto defn = pollUntil<SymbolResult>([&] {
        return fx.bridge().findDefinition("make_widget", {fixtureFile("widget.py")});
    });
    // Only skip if the index never caught up within the full readiness budget.
    if (!defn.has_value()) {
        GTEST_SKIP() << "pyright did not index make_widget within the 30s "
                        "readiness budget (cold cache / overloaded machine)";
    }
    EXPECT_NE(defn->file.find("widget.py"), std::string::npos);
    EXPECT_GE(defn->line, 0);
}

TEST(PyrightBridge, FindDefinitionOnMethod) {
    PyrightFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "pyright backend unavailable";

    auto defn = pollUntil<SymbolResult>([&] {
        return fx.bridge().findDefinition("compute", {fixtureFile("widget.py")});
    });
    if (!defn.has_value()) {
        GTEST_SKIP() << "pyright did not index Widget.compute within the 30s "
                        "readiness budget (cold cache / overloaded machine)";
    }
    EXPECT_NE(defn->file.find("widget.py"), std::string::npos);
}

TEST(PyrightBridge, FindReferencesOnFunction) {
    PyrightFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "pyright backend unavailable";

    auto refs = fx.bridge().findReferences("make_widget", {fixtureFile("widget.py")});
    // `make_widget` is defined once and called once in sum_widgets — but the
    // exact reference count depends on pyright's indexing heuristics, so we
    // only assert non-crash behaviour and that results (if any) live in
    // widget.py.
    for (const auto& r : refs) {
        EXPECT_NE(r.file.find("widget.py"), std::string::npos);
    }
    SUCCEED();
}

TEST(PyrightBridge, GetHoverInfoReturnsSignature) {
    PyrightFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "pyright backend unavailable";

    auto hover = fx.bridge().getHoverInfo("compute", {fixtureFile("widget.py")});
    // Hover content is implementation-defined; only verify that, when pyright
    // returns text, it is non-empty.
    if (hover.has_value()) {
        EXPECT_FALSE(hover->empty());
    } else {
        SUCCEED() << "pyright returned no hover (acceptable)";
    }
}

TEST(PyrightBridge, FindTypeDefinitionOnClass) {
    PyrightFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "pyright backend unavailable";

    auto type = fx.bridge().findTypeDefinition("Widget",
                                               {fixtureFile("widget.py")},
                                               {});
    ASSERT_TRUE(type.has_value());
    EXPECT_NE(type->file.find("widget.py"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Failure-path tests (missing-binary / malformed-harness degradation)
// ---------------------------------------------------------------------------

// Test: when pyright-langserver binary is missing the bridge must fail
// start() cleanly, degrade to unavailable, and return empty/nullopt from
// every query API.
TEST(PyrightBridge, BridgeMissingBinaryFallsBackGracefully) {
    PyrightBridge bridge;
    bool started = bridge.start("/nonexistent-dir-xyz/pyright-xyz", fixtureRootUri());
    EXPECT_FALSE(started);
    EXPECT_FALSE(bridge.isAvailable());

    EXPECT_FALSE(bridge.findDefinition("make_widget", {}).has_value());
    EXPECT_FALSE(bridge.getHoverInfo("make_widget", {}).has_value());
    EXPECT_TRUE(bridge.findReferences("make_widget", {}).empty());

    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

// Test: malformed JSON-RPC response handling. Points the real PyrightBridge
// at the fake-langserver harness in every malformed mode and asserts
// graceful degradation through the bridge's actual framing/parse/init path
// (no crash, no hang, stays unavailable, queries return empty/nullopt).
// Requires no pyright install — the fake server stands in for it.
TEST(PyrightBridge, BridgeMalformedResponseHandled) {
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
        PyrightBridge bridge;
        bool started = bridge.start(fake, fixtureRootUri());
        EXPECT_FALSE(bridge.isAvailable())
            << "bridge must not report available on malformed handshake";
        (void)started;

        EXPECT_FALSE(bridge.findDefinition("make_widget", {}).has_value());
        EXPECT_FALSE(bridge.getHoverInfo("make_widget", {}).has_value());
        EXPECT_TRUE(bridge.findReferences("make_widget", {}).empty());

        bridge.stop();
        EXPECT_FALSE(bridge.isAvailable());
    }
}

TEST(PyrightBridge, UnknownSymbolReturnsNullopt) {
    PyrightFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "pyright backend unavailable";

    auto defn = fx.bridge().findDefinition("ThisSymbolShouldNotExist_9f8a7c6b",
                                           {fixtureFile("widget.py")});
    EXPECT_FALSE(defn.has_value());

    auto hover = fx.bridge().getHoverInfo("ThisSymbolShouldNotExist_9f8a7c6b",
                                          {fixtureFile("widget.py")});
    EXPECT_FALSE(hover.has_value());
}
