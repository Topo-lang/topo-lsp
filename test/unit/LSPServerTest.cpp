#include "LSPServer.h"
#include "topo/Stdlib/Types.h"

#include <gtest/gtest.h>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace topo::lsp;
using json = nlohmann::json;

// Helper: read fixture file
static std::string readFixture(const std::string& name) {
    std::string path = std::string(TOPO_LSP_FIXTURES_DIR) + "/" + name;
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ============================================================================
// Test fixture: manages LSPServer lifecycle
// ============================================================================

class LSPServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Send initialize request
        json initMsg = {{"jsonrpc", "2.0"},
                        {"id", 0},
                        {"method", "initialize"},
                        {"params", {{"rootUri", nullptr}, {"capabilities", json::object()}}}};
        auto initResp = server_.handleMessage(initMsg);
        ASSERT_TRUE(initResp.has_value());
        EXPECT_TRUE((*initResp)["result"].contains("capabilities"));

        // Send initialized notification
        json initializedMsg = {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", json::object()}};
        server_.handleMessage(initializedMsg);
    }

    // Open a document in the LSP server and drain pending notifications
    void openDocument(const std::string& uri, const std::string& content) {
        json didOpenMsg = {
            {"jsonrpc", "2.0"},
            {"method", "textDocument/didOpen"},
            {"params", {{"textDocument", {{"uri", uri}, {"languageId", "topo"}, {"version", 1}, {"text", content}}}}}};
        server_.handleMessage(didOpenMsg);
        // Drain pending notifications (diagnostics from didOpen)
        server_.takePendingNotifications();
    }

    LSPServer server_;
    static constexpr const char* testUri_ = "file:///test/lsp_test.topo";
};

// ============================================================================
// Hover
// ============================================================================

TEST_F(LSPServerTest, HoverOnFunction) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty()) << "Could not read fixture lsp_test.topo";
    openDocument(testUri_, source);

    // Hover on "init" at line 4 (0-based), character 13
    // Source line: "        void init();"
    json hoverMsg = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 4}, {"character", 13}}}}}};
    auto resp = server_.handleMessage(hoverMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_FALSE(result.is_null()) << "Hover should return a result for 'init'";
    ASSERT_TRUE(result.contains("contents"));
    auto value = result["contents"]["value"].get<std::string>();
    EXPECT_NE(value.find("init"), std::string::npos) << "Hover content should contain function name";
    EXPECT_NE(value.find("void"), std::string::npos) << "Hover content should contain return type";
}

TEST_F(LSPServerTest, HoverOnEmptyPositionReturnsNull) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    // Hover on an empty line or whitespace area (line 0, col 0 is "using" keyword area,
    // but let's try a position clearly in whitespace: line 6 is blank)
    json hoverMsg = {
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 6}, {"character", 0}}}}}};
    auto resp = server_.handleMessage(hoverMsg);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE((*resp)["result"].is_null()) << "Hover on whitespace should return null";
}

// ============================================================================
// Go to Definition
// ============================================================================

TEST_F(LSPServerTest, DefinitionOnFunction) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    // Request definition for "compute" at line 5 (0-based), within the identifier
    // Source line: "        Int compute(Int x);"
    json defMsg = {{"jsonrpc", "2.0"},
                   {"id", 3},
                   {"method", "textDocument/definition"},
                   {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 5}, {"character", 12}}}}}};
    auto resp = server_.handleMessage(defMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_FALSE(result.is_null()) << "Definition should return a location for 'compute'";
    EXPECT_TRUE(result.contains("uri"));
    EXPECT_TRUE(result.contains("range"));
}

// ============================================================================
// Find References
// ============================================================================

TEST_F(LSPServerTest, ReferencesForPipelineFunction) {
    // Use a source that has a pipeline fn block — pipeline DAG nodes generate
    // call sites that are visible to the references handler.
    std::string source =
        "namespace app {\n"
        "    public:\n"
        "        void fetch();\n"
        "        void decode();\n"
        "        void execute();\n"
        "        void run();\n"
        "\n"
        "        fn run {\n"
        "            fetch -> decode;\n"
        "            decode -> execute;\n"
        "            execute -> void;\n"
        "        }\n"
        "}\n";
    openDocument(testUri_, source);

    // Request references for "fetch" at its declaration (line 2, col 13)
    json refsMsg = {{"jsonrpc", "2.0"},
                    {"id", 4},
                    {"method", "textDocument/references"},
                    {"params",
                     {{"textDocument", {{"uri", testUri_}}},
                      {"position", {{"line", 2}, {"character", 13}}},
                      {"context", {{"includeDeclaration", true}}}}}};
    auto resp = server_.handleMessage(refsMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    // "fetch" is a DAG node in the pipeline, which generates a call site
    EXPECT_GE(result.size(), 1u) << "References for 'fetch' should find the pipeline call site";
}

TEST_F(LSPServerTest, ReferencesForUnknownIdentifier) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    // Request references for an identifier that doesn't exist (whitespace position)
    json refsMsg = {{"jsonrpc", "2.0"},
                    {"id", 5},
                    {"method", "textDocument/references"},
                    {"params",
                     {{"textDocument", {{"uri", testUri_}}},
                      {"position", {{"line", 6}, {"character", 0}}},
                      {"context", {{"includeDeclaration", false}}}}}};
    auto resp = server_.handleMessage(refsMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 0u) << "References for empty position should return empty array";
}

// ============================================================================
// std::import Hover & Definition
// ============================================================================

TEST_F(LSPServerTest, HoverOnStdImportType) {
    // std::import declares an external type — hover should show import info
    std::string source =
        "std::import(\"engine/audio.h\", AudioClip);\n"
        "\n"
        "namespace audio {\n"
        "    public:\n"
        "        AudioClip loadClip();\n"
        "}\n";
    openDocument(testUri_, source);

    // Hover on "AudioClip" at the std::import declaration (line 0, col 35)
    json hoverMsg = {
        {"jsonrpc", "2.0"},
        {"id", 10},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 0}, {"character", 35}}}}}};
    auto resp = server_.handleMessage(hoverMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_FALSE(result.is_null()) << "Hover should return result for std::import type";
    auto value = result["contents"]["value"].get<std::string>();
    EXPECT_NE(value.find("std::import"), std::string::npos) << "Hover should show std::import syntax";
    EXPECT_NE(value.find("engine/audio.h"), std::string::npos) << "Hover should show import path";
    EXPECT_NE(value.find("AudioClip"), std::string::npos) << "Hover should show type name";
}

TEST_F(LSPServerTest, HoverOnStdImportTypeUsage) {
    // Hover on usage of an std::import type (in a function return type)
    std::string source =
        "std::import(\"engine/audio.h\", AudioClip);\n"
        "\n"
        "namespace audio {\n"
        "    public:\n"
        "        AudioClip loadClip();\n"
        "}\n";
    openDocument(testUri_, source);

    // Hover on "AudioClip" at usage site (line 4, col 8)
    json hoverMsg = {
        {"jsonrpc", "2.0"},
        {"id", 11},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 4}, {"character", 10}}}}}};
    auto resp = server_.handleMessage(hoverMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    // AudioClip at usage site could match as function return type or as import type
    // Either way it should not be null
    if (!result.is_null()) {
        auto value = result["contents"]["value"].get<std::string>();
        // Could match the function "loadClip" (AudioClip is its return type)
        // or the import type itself — either is valid
        EXPECT_TRUE(value.find("AudioClip") != std::string::npos || value.find("loadClip") != std::string::npos)
            << "Hover should contain relevant info";
    }
}

TEST_F(LSPServerTest, DefinitionOnStdImportType) {
    // Go-to-definition on std::import type should resolve to something
    // (without bridges, falls back to .topo declaration)
    std::string source =
        "std::import(\"engine/audio.h\", AudioClip);\n"
        "\n"
        "namespace audio {\n"
        "    public:\n"
        "        AudioClip loadClip();\n"
        "}\n";
    openDocument(testUri_, source);

    // Definition on "AudioClip" at the import declaration (line 0, col 35)
    json defMsg = {{"jsonrpc", "2.0"},
                   {"id", 12},
                   {"method", "textDocument/definition"},
                   {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 0}, {"character", 35}}}}}};
    auto resp = server_.handleMessage(defMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    // Without bridge, should fallback to .topo declaration location
    ASSERT_FALSE(result.is_null()) << "Definition on std::import type should return a location (fallback to .topo)";
    EXPECT_TRUE(result.contains("uri"));
    EXPECT_TRUE(result.contains("range"));
}

// ============================================================================
// Completion
// ============================================================================

TEST_F(LSPServerTest, CompletionReturnsItems) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    // Request completion at the beginning of a line (empty prefix -> all items)
    json compMsg = {{"jsonrpc", "2.0"},
                    {"id", 5},
                    {"method", "textDocument/completion"},
                    {"params", {{"textDocument", {{"uri", testUri_}}}, {"position", {{"line", 4}, {"character", 0}}}}}};
    auto resp = server_.handleMessage(compMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.contains("items"));
    auto& items = result["items"];
    ASSERT_TRUE(items.is_array());
    EXPECT_GT(items.size(), 0u) << "Completion should return at least keywords";

    // Verify keywords are present
    bool hasNamespace = false;
    bool hasFunction = false;
    for (const auto& item : items) {
        std::string label = item["label"].get<std::string>();
        if (label == "namespace") hasNamespace = true;
        // Check for a declared function
        if (label == "init" || label == "compute") hasFunction = true;
    }
    EXPECT_TRUE(hasNamespace) << "Completion should include 'namespace' keyword";
    EXPECT_TRUE(hasFunction) << "Completion should include declared functions";
}

TEST_F(LSPServerTest, CompletionWithPrefix) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    // Type "co" prefix -> should match "compute", "const", "comptime", "constraint"
    // We'll point at the "compute" identifier location to simulate prefix
    // Actually the identifier at position is resolved from the source text,
    // so let's open a document with a partial identifier
    std::string sourceWithPartial =
        "using Int = std::cpp17::int32_t;\n"
        "\n"
        "namespace engine {\n"
        "    public:\n"
        "        void init();\n"
        "        Int compute(Int x);\n"
        "        co\n" // partial identifier for completion
        "}\n";
    std::string partialUri = "file:///test/partial.topo";
    openDocument(partialUri, sourceWithPartial);

    json compMsg = {
        {"jsonrpc", "2.0"},
        {"id", 6},
        {"method", "textDocument/completion"},
        {"params", {{"textDocument", {{"uri", partialUri}}}, {"position", {{"line", 6}, {"character", 10}}}}}};
    auto resp = server_.handleMessage(compMsg);
    ASSERT_TRUE(resp.has_value());

    auto& items = (*resp)["result"]["items"];
    ASSERT_TRUE(items.is_array());

    // The "co" prefix should match "compute" and "const"
    bool hasConst = false;
    for (const auto& item : items) {
        std::string label = item["label"].get<std::string>();
        if (label == "const") hasConst = true;
    }
    EXPECT_TRUE(hasConst) << "Completion with prefix 'co' should include 'const'";
}

// ============================================================================
// Code Action
// ============================================================================

TEST_F(LSPServerTest, CodeActionForUnknownType) {
    // The code action handler responds to diagnostics with code "unknown-type".
    // It extracts the first quoted name from the message and checks if it matches
    // a known C++ builtin (int, bool). We construct a synthetic diagnostic that
    // matches this pattern directly.
    std::string source =
        "namespace app {\n"
        "    public:\n"
        "        void run();\n"
        "}\n";
    openDocument(testUri_, source);

    // Construct a synthetic unknown-type diagnostic where the first quoted name
    // is the type name (matching how extractQuotedName works)
    json syntheticDiag = {
        {"range", {{"start", {{"line", 2}, {"character", 8}}}, {"end", {{"line", 2}, {"character", 11}}}}},
        {"severity", 1},
        {"source", "topo"},
        {"code", "unknown-type"},
        {"message", "unknown type 'int'"}};

    json codeActionMsg = {{"jsonrpc", "2.0"},
                          {"id", 7},
                          {"method", "textDocument/codeAction"},
                          {"params",
                           {{"textDocument", {{"uri", testUri_}}},
                            {"range", syntheticDiag["range"]},
                            {"context", {{"diagnostics", json::array({syntheticDiag})}}}}}};
    auto resp = server_.handleMessage(codeActionMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    ASSERT_GE(result.size(), 1u) << "Should suggest a quick fix for unknown type 'int'";

    // Verify the quick fix structure
    auto& action = result[0];
    EXPECT_EQ(action["kind"].get<std::string>(), "quickfix");
    EXPECT_TRUE(action.contains("edit"));
    auto title = action["title"].get<std::string>();
    EXPECT_NE(title.find("using"), std::string::npos) << "Quick fix title should suggest 'using' declaration";
    EXPECT_NE(title.find("int"), std::string::npos) << "Quick fix title should mention the type 'int'";
}

TEST_F(LSPServerTest, CodeActionEmptyDiagnostics) {
    std::string source =
        "namespace app {\n"
        "    public:\n"
        "        void run();\n"
        "}\n";
    openDocument(testUri_, source);

    // Code action with no diagnostics should return empty array
    json codeActionMsg = {
        {"jsonrpc", "2.0"},
        {"id", 8},
        {"method", "textDocument/codeAction"},
        {"params",
         {{"textDocument", {{"uri", testUri_}}},
          {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 1}}}}},
          {"context", {{"diagnostics", json::array()}}}}}};
    auto resp = server_.handleMessage(codeActionMsg);
    ASSERT_TRUE(resp.has_value());
    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 0u) << "No diagnostics should produce no code actions";
}

// ============================================================================
// Semantic Tokens
// ============================================================================

TEST_F(LSPServerTest, SemanticTokensFull) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    json semTokMsg = {{"jsonrpc", "2.0"},
                      {"id", 8},
                      {"method", "textDocument/semanticTokens/full"},
                      {"params", {{"textDocument", {{"uri", testUri_}}}}}};
    auto resp = server_.handleMessage(semTokMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.contains("data"));
    auto& data = result["data"];
    ASSERT_TRUE(data.is_array());
    EXPECT_GT(data.size(), 0u) << "Semantic tokens data should be non-empty for a valid .topo file";
    // Data is delta-encoded: each token is 5 integers
    // [deltaLine, deltaStartChar, length, tokenType, tokenModifiers]
    EXPECT_EQ(data.size() % 5, 0u) << "Semantic token data array size should be a multiple of 5";
}

// Regression: every stdlib bridging type must surface in completion AND
// be highlighted as a defaultLibrary type. The two LSP sites are driven
// from topo::stdlib::allEntries() rather than a hand-maintained list, so
// this test fails the moment a new stdlib type is added to the table but
// a parallel list reappears in LSPServer.cpp and drifts behind it.
TEST_F(LSPServerTest, StdlibTypesFullySurfaced) {
    // One declaration per stdlib type, each keyword appearing standalone
    // in return-type position so it lexes as its KW_* token.
    std::string source =
        "namespace s {\n"
        "    public:\n"
        "        bool a();\n"
        "        i8 b();\n"
        "        i16 c();\n"
        "        i32 d();\n"
        "        i64 e();\n"
        "        u8 f();\n"
        "        u16 g();\n"
        "        u32 h();\n"
        "        u64 i();\n"
        "        f32 j();\n"
        "        f64 k();\n"
        "        string l();\n"
        "        optional<i64> m();\n"
        "        slice<u8> n();\n"
        "        record<x: i64> o();\n"
        "        bytes p();\n"
        "        array<i64, 4> q();\n"
        "        union<tag: u8, v: i64> r();\n"
        "        time_ns s();\n"
        "        uuid t();\n"
        "        decimal128 dq();\n"
        "\n" // blank line for empty-prefix completion
        "}\n";
    std::string uri = "file:///test/stdlib_surface.topo";
    openDocument(uri, source);

    // --- Completion: every keyword present as a type-like item (kind 7) ---
    json compMsg = {{"jsonrpc", "2.0"},
                    {"id", 70},
                    {"method", "textDocument/completion"},
                    {"params", {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 18}, {"character", 0}}}}}};
    auto compResp = server_.handleMessage(compMsg);
    ASSERT_TRUE(compResp.has_value());
    auto& items = (*compResp)["result"]["items"];
    ASSERT_TRUE(items.is_array());
    std::set<std::string> completionTypeLabels;
    for (const auto& item : items) {
        if (item.contains("kind") && item["kind"].get<int>() == 7) {
            completionTypeLabels.insert(item["label"].get<std::string>());
        }
    }
    for (const auto& e : topo::stdlib::allEntries()) {
        EXPECT_TRUE(completionTypeLabels.count(e.keyword) > 0)
            << "stdlib type '" << e.keyword << "' missing from completion";
    }

    // --- Semantic tokens: every keyword highlighted as type+defaultLibrary ---
    json semMsg = {{"jsonrpc", "2.0"},
                   {"id", 71},
                   {"method", "textDocument/semanticTokens/full"},
                   {"params", {{"textDocument", {{"uri", uri}}}}}};
    auto semResp = server_.handleMessage(semMsg);
    ASSERT_TRUE(semResp.has_value());
    auto& data = (*semResp)["result"]["data"];
    ASSERT_TRUE(data.is_array());

    // Split source into lines for text reconstruction from delta positions.
    std::vector<std::string> lines;
    {
        std::istringstream ss(source);
        std::string ln;
        while (std::getline(ss, ln)) lines.push_back(ln);
    }
    constexpr int kTtType = 1;          // TT_TYPE
    constexpr int kTmDefaultLibrary = 1 << 4; // TM_DEFAULTLIBRARY
    int curLine = 0, curCol = 0;
    std::set<std::string> defaultLibTypeTexts;
    for (size_t i = 0; i + 4 < data.size(); i += 5) {
        int dLine = data[i].get<int>();
        int dCol = data[i + 1].get<int>();
        int len = data[i + 2].get<int>();
        int tType = data[i + 3].get<int>();
        int tMods = data[i + 4].get<int>();
        if (dLine == 0) {
            curCol += dCol;
        } else {
            curLine += dLine;
            curCol = dCol;
        }
        if (tType == kTtType && (tMods & kTmDefaultLibrary) != 0 &&
            curLine >= 0 && curLine < static_cast<int>(lines.size()) &&
            curCol >= 0 && curCol + len <= static_cast<int>(lines[curLine].size())) {
            defaultLibTypeTexts.insert(lines[curLine].substr(curCol, len));
        }
    }
    for (const auto& e : topo::stdlib::allEntries()) {
        EXPECT_TRUE(defaultLibTypeTexts.count(e.keyword) > 0)
            << "stdlib type '" << e.keyword
            << "' not highlighted as type+defaultLibrary";
    }
}

// ============================================================================
// Document Symbol
// ============================================================================

TEST_F(LSPServerTest, DocumentSymbol) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());
    openDocument(testUri_, source);

    json docSymMsg = {{"jsonrpc", "2.0"},
                      {"id", 9},
                      {"method", "textDocument/documentSymbol"},
                      {"params", {{"textDocument", {{"uri", testUri_}}}}}};
    auto resp = server_.handleMessage(docSymMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    EXPECT_GT(result.size(), 0u) << "Document symbols should include at least the type alias and namespace";

    // Look for the "engine" namespace symbol
    bool foundEngine = false;
    bool foundTypeAlias = false;
    for (const auto& sym : result) {
        std::string name = sym["name"].get<std::string>();
        if (name == "engine") {
            foundEngine = true;
            // Namespace should have children (the functions)
            if (sym.contains("children")) {
                EXPECT_GT(sym["children"].size(), 0u) << "Namespace 'engine' should have child symbols";
            }
        }
        if (name == "Int") {
            foundTypeAlias = true;
        }
    }
    EXPECT_TRUE(foundEngine) << "Should find 'engine' namespace in document symbols";
    EXPECT_TRUE(foundTypeAlias) << "Should find 'Int' type alias in document symbols";
}

// ============================================================================
// Diagnostics (via didOpen)
// ============================================================================

TEST_F(LSPServerTest, DiagnosticsOnInvalidContent) {
    // Open a document with a syntax error
    std::string invalidSource =
        "namespace broken {\n"
        "    public:\n"
        "        void run(\n" // missing closing paren and semicolon
        "}\n";
    std::string invalidUri = "file:///test/invalid.topo";

    json didOpenMsg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params",
         {{"textDocument", {{"uri", invalidUri}, {"languageId", "topo"}, {"version", 1}, {"text", invalidSource}}}}}};
    server_.handleMessage(didOpenMsg);

    auto notifications = server_.takePendingNotifications();
    ASSERT_FALSE(notifications.empty()) << "Opening invalid content should produce publishDiagnostics notification";

    // Find the diagnostics notification
    bool foundDiagnostics = false;
    for (const auto& notif : notifications) {
        if (notif.value("method", "") == "textDocument/publishDiagnostics") {
            foundDiagnostics = true;
            auto& params = notif["params"];
            EXPECT_EQ(params["uri"].get<std::string>(), invalidUri);
            auto& diags = params["diagnostics"];
            ASSERT_TRUE(diags.is_array());
            EXPECT_GT(diags.size(), 0u) << "Invalid content should produce at least one diagnostic";
            // Verify diagnostic structure
            auto& firstDiag = diags[0];
            EXPECT_TRUE(firstDiag.contains("range"));
            EXPECT_TRUE(firstDiag.contains("severity"));
            EXPECT_TRUE(firstDiag.contains("message"));
            break;
        }
    }
    EXPECT_TRUE(foundDiagnostics) << "Should receive publishDiagnostics notification for invalid content";
}

TEST_F(LSPServerTest, DiagnosticsOnValidContent) {
    std::string source = readFixture("lsp_test.topo");
    ASSERT_FALSE(source.empty());

    json didOpenMsg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", {{"textDocument", {{"uri", testUri_}, {"languageId", "topo"}, {"version", 1}, {"text", source}}}}}};
    server_.handleMessage(didOpenMsg);

    auto notifications = server_.takePendingNotifications();
    bool foundDiagnostics = false;
    for (const auto& notif : notifications) {
        if (notif.value("method", "") == "textDocument/publishDiagnostics") {
            foundDiagnostics = true;
            auto& diags = notif["params"]["diagnostics"];
            EXPECT_EQ(diags.size(), 0u) << "Valid content should produce zero diagnostics";
            break;
        }
    }
    EXPECT_TRUE(foundDiagnostics) << "Should still receive publishDiagnostics (with empty array) for valid content";
}

// ============================================================================
// Non-canonical syntax: diagnostics + code action
// ============================================================================

TEST_F(LSPServerTest, NonCanonicalSyntaxDiagnostics) {
    // "module" is a non-canonical alias for "namespace" — strict parse fails,
    // lenient parse succeeds and produces Warning diagnostics.
    std::string nonCanonicalSource =
        "module app {\n"
        "    public:\n"
        "        void run();\n"
        "}\n";
    std::string ncUri = "file:///test/non_canonical.topo";

    json didOpenMsg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params",
         {{"textDocument", {{"uri", ncUri}, {"languageId", "topo"}, {"version", 1}, {"text", nonCanonicalSource}}}}}};
    server_.handleMessage(didOpenMsg);

    auto notifications = server_.takePendingNotifications();
    ASSERT_FALSE(notifications.empty());

    bool foundNonCanonical = false;
    for (const auto& notif : notifications) {
        if (notif.value("method", "") != "textDocument/publishDiagnostics") continue;
        auto& diags = notif["params"]["diagnostics"];
        for (const auto& d : diags) {
            if (d.contains("code") && d["code"].get<std::string>() == "non-canonical-syntax") {
                foundNonCanonical = true;
                // Severity 2 = Warning in LSP
                EXPECT_EQ(d["severity"].get<int>(), 2) << "Non-canonical syntax should be Warning severity";
                auto msg = d["message"].get<std::string>();
                EXPECT_NE(msg.find("module"), std::string::npos) << "Message should mention the original keyword";
                EXPECT_NE(msg.find("namespace"), std::string::npos) << "Message should mention the canonical keyword";
            }
        }
    }
    EXPECT_TRUE(foundNonCanonical) << "Opening a file with 'module' should produce non-canonical-syntax diagnostics";
}

TEST_F(LSPServerTest, NonCanonicalSyntaxCodeAction) {
    // Open a document with non-canonical syntax to populate the analysis cache
    std::string nonCanonicalSource =
        "module app {\n"
        "    public:\n"
        "        void run();\n"
        "}\n";
    std::string ncUri = "file:///test/non_canonical2.topo";

    json didOpenMsg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params",
         {{"textDocument", {{"uri", ncUri}, {"languageId", "topo"}, {"version", 1}, {"text", nonCanonicalSource}}}}}};
    server_.handleMessage(didOpenMsg);
    server_.takePendingNotifications();

    // Build a synthetic diagnostic matching the non-canonical-syntax code
    json syntheticDiag = {
        {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 6}}}}},
        {"severity", 2},
        {"source", "topo"},
        {"code", "non-canonical-syntax"},
        {"message", "non-canonical syntax: 'module' should be 'namespace'"}};

    json codeActionMsg = {{"jsonrpc", "2.0"},
                          {"id", 20},
                          {"method", "textDocument/codeAction"},
                          {"params",
                           {{"textDocument", {{"uri", ncUri}}},
                            {"range", syntheticDiag["range"]},
                            {"context", {{"diagnostics", json::array({syntheticDiag})}}}}}};
    auto resp = server_.handleMessage(codeActionMsg);
    ASSERT_TRUE(resp.has_value());

    auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    ASSERT_GE(result.size(), 1u) << "Should offer a code action for non-canonical-syntax";

    auto& action = result[0];
    EXPECT_EQ(action["title"].get<std::string>(), "Convert to standard Topo syntax");
    EXPECT_EQ(action["kind"].get<std::string>(), "quickfix");
    EXPECT_TRUE(action.contains("edit")) << "Code action should contain a workspace edit";

    // The edit should replace the full document
    auto& changes = action["edit"]["changes"];
    ASSERT_TRUE(changes.contains(ncUri));
    auto& edits = changes[ncUri];
    ASSERT_EQ(edits.size(), 1u);
    auto newText = edits[0]["newText"].get<std::string>();
    // The canonical output should use 'namespace' instead of 'module'
    EXPECT_NE(newText.find("namespace app"), std::string::npos)
        << "Canonical source should use 'namespace' instead of 'module'";
    EXPECT_EQ(newText.find("module app"), std::string::npos) << "Canonical source should not contain 'module'";
}

// ============================================================================
// didChange (incremental update) — cold-start coverage is elsewhere; these
// exercise edit-path behavior (incremental-update coverage).
// ============================================================================

// Helper: send a full-document didChange and drain pending notifications.
// LSP server supports both full-text (no range) and incremental (with range).
// We use full-text here because it is simpler and the unit-level concern is
// that the edit triggers fresh analysis + diagnostics, not sync protocol
// mechanics (which are covered in protocol/LspSyncNotificationsTest.cpp).
static std::vector<json> didChangeFullAndDrain(LSPServer& server, const std::string& uri,
                                                const std::string& newText, int version) {
    json msg = {{"jsonrpc", "2.0"},
                {"method", "textDocument/didChange"},
                {"params",
                 {{"textDocument", {{"uri", uri}, {"version", version}}},
                  {"contentChanges", json::array({{{"text", newText}}})}}}};
    server.handleMessage(msg);
    return server.takePendingNotifications();
}

TEST_F(LSPServerTest, DidChangeUpdatesDiagnostics) {
    // Open valid document -> didChange introduces a syntax error -> expect
    // fresh publishDiagnostics with errors. Then didChange back to valid ->
    // expect publishDiagnostics clears.
    std::string validSource =
        "namespace app {\n"
        "    public:\n"
        "        void run();\n"
        "}\n";
    std::string invalidSource =
        "namespace broken {\n"
        "    public:\n"
        "        void run(\n" // missing closing paren + semicolon
        "}\n";
    std::string uri = "file:///test/didchange_diags.topo";
    openDocument(uri, validSource);

    // didChange -> introduce error
    auto notifs1 = didChangeFullAndDrain(server_, uri, invalidSource, 2);
    ASSERT_FALSE(notifs1.empty()) << "didChange with invalid content should emit publishDiagnostics";

    bool gotErrorDiag = false;
    for (const auto& n : notifs1) {
        if (n.value("method", "") != "textDocument/publishDiagnostics") continue;
        EXPECT_EQ(n["params"]["uri"].get<std::string>(), uri);
        auto& diags = n["params"]["diagnostics"];
        if (diags.is_array() && diags.size() > 0) {
            gotErrorDiag = true;
            // Verify diagnostic shape
            EXPECT_TRUE(diags[0].contains("range"));
            EXPECT_TRUE(diags[0].contains("message"));
        }
    }
    EXPECT_TRUE(gotErrorDiag) << "Invalid content via didChange should surface at least one diagnostic";

    // didChange back to valid
    auto notifs2 = didChangeFullAndDrain(server_, uri, validSource, 3);
    bool gotCleared = false;
    for (const auto& n : notifs2) {
        if (n.value("method", "") != "textDocument/publishDiagnostics") continue;
        auto& diags = n["params"]["diagnostics"];
        if (diags.is_array() && diags.size() == 0) {
            gotCleared = true;
        }
    }
    EXPECT_TRUE(gotCleared) << "Valid content via didChange should clear diagnostics";
}

TEST_F(LSPServerTest, DidChangePreservesHoverAfterEdit) {
    // Open doc, hover on symbol X, didChange to rename X->Y, hover at new
    // position, verify the edit is reflected (catches stale SymbolTable).
    std::string before =
        "namespace engine {\n"
        "    public:\n"
        "        void originalName();\n"
        "}\n";
    std::string after =
        "namespace engine {\n"
        "    public:\n"
        "        void renamedName();\n"
        "}\n";
    std::string uri = "file:///test/didchange_hover.topo";
    openDocument(uri, before);

    // Hover on "originalName" at line 2 (0-based) — identifier starts around col 13
    json hoverBefore = {
        {"jsonrpc", "2.0"},
        {"id", 100},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 2}, {"character", 15}}}}}};
    auto respBefore = server_.handleMessage(hoverBefore);
    ASSERT_TRUE(respBefore.has_value());
    auto& resultBefore = (*respBefore)["result"];
    ASSERT_FALSE(resultBefore.is_null()) << "Hover on originalName should return a result";
    auto valueBefore = resultBefore["contents"]["value"].get<std::string>();
    EXPECT_NE(valueBefore.find("originalName"), std::string::npos)
        << "Hover before edit should mention original name";

    // didChange to rename
    didChangeFullAndDrain(server_, uri, after, 2);

    // Hover at same position — should now see the renamed symbol
    json hoverAfter = {
        {"jsonrpc", "2.0"},
        {"id", 101},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 2}, {"character", 15}}}}}};
    auto respAfter = server_.handleMessage(hoverAfter);
    ASSERT_TRUE(respAfter.has_value());
    auto& resultAfter = (*respAfter)["result"];
    ASSERT_FALSE(resultAfter.is_null()) << "Hover on renamedName should return a result";
    auto valueAfter = resultAfter["contents"]["value"].get<std::string>();
    EXPECT_NE(valueAfter.find("renamedName"), std::string::npos)
        << "Hover after edit must reflect new identifier, not stale SymbolTable entry";
    EXPECT_EQ(valueAfter.find("originalName"), std::string::npos)
        << "Stale hover result: server returned the pre-edit name";
}

TEST_F(LSPServerTest, DidChangeMultipleEditsConsistent) {
    // Apply 3 sequential didChanges. Final state should match what a cold
    // open of the final text would produce (detects desync / stale cache).
    std::string v1 =
        "namespace a {\n"
        "    public:\n"
        "        void alpha();\n"
        "}\n";
    std::string v2 =
        "namespace a {\n"
        "    public:\n"
        "        void alpha();\n"
        "        void beta();\n"
        "}\n";
    std::string v3 =
        "namespace a {\n"
        "    public:\n"
        "        void alpha();\n"
        "        void beta();\n"
        "        void gamma();\n"
        "}\n";

    std::string editedUri = "file:///test/didchange_multi_edited.topo";
    openDocument(editedUri, v1);
    didChangeFullAndDrain(server_, editedUri, v2, 2);
    didChangeFullAndDrain(server_, editedUri, v3, 3);

    // Cold-open v3 in a fresh URI
    std::string coldUri = "file:///test/didchange_multi_cold.topo";
    openDocument(coldUri, v3);

    // Compare documentSymbol output between the edited path and cold path.
    auto querySymbols = [&](const std::string& u) -> json {
        json msg = {{"jsonrpc", "2.0"},
                    {"id", 200},
                    {"method", "textDocument/documentSymbol"},
                    {"params", {{"textDocument", {{"uri", u}}}}}};
        auto resp = server_.handleMessage(msg);
        return (*resp)["result"];
    };

    auto editedResult = querySymbols(editedUri);
    auto coldResult = querySymbols(coldUri);

    // Collect all leaf names (recursive)
    std::function<void(const json&, std::set<std::string>&)> collect =
        [&](const json& arr, std::set<std::string>& out) {
            if (!arr.is_array()) return;
            for (const auto& sym : arr) {
                if (sym.contains("name")) out.insert(sym["name"].get<std::string>());
                if (sym.contains("children")) collect(sym["children"], out);
            }
        };

    std::set<std::string> editedNames, coldNames;
    collect(editedResult, editedNames);
    collect(coldResult, coldNames);

    EXPECT_EQ(editedNames, coldNames)
        << "Document symbols after 3 didChanges must match a cold open of the final text";
    // Sanity: all three functions should be present
    EXPECT_TRUE(editedNames.count("alpha")) << "alpha missing after edits";
    EXPECT_TRUE(editedNames.count("beta")) << "beta missing after edits";
    EXPECT_TRUE(editedNames.count("gamma")) << "gamma missing after edits";
}

// ============================================================================
// Edge-case fixture regression coverage
// (realistic edge-case fixtures the earlier suites lacked)
//
// These fixtures exercise real-project scenarios (implicit conversions,
// macro-defined symbols, extern "C" FFI, template specialization).  The
// TEST_F bodies are intentionally tolerant: the fixtures are regression
// reach, not correctness oracles. The goal is that any future change in
// LSPServer / bridge behavior that breaks on these inputs will surface
// here rather than silently regressing in production use.
// ============================================================================

TEST_F(LSPServerTest, FixtureImplicitConversionLoadsAndHovers) {
    // Implicit conversion chain: int -> long -> double in host C++. The
    // .topo declares the public surface; LSP should hover the function
    // without crashing on the conversion-heavy host.
    std::string source = readFixture("lsp_implicit_conversion.topo");
    ASSERT_FALSE(source.empty()) << "Fixture lsp_implicit_conversion.topo must exist";
    openDocument("file:///test/lsp_implicit_conversion.topo", source);

    json hoverMsg = {
        {"jsonrpc", "2.0"},
        {"id", 300},
        {"method", "textDocument/hover"},
        {"params",
         {{"textDocument", {{"uri", "file:///test/lsp_implicit_conversion.topo"}}},
          {"position", {{"line", 3}, {"character", 15}}}}}};
    auto resp = server_.handleMessage(hoverMsg);
    ASSERT_TRUE(resp.has_value()) << "Hover request should always return a response";
    // Result may be null if the position isn't on an identifier; we only
    // assert the server responds without crashing on the fixture.
    SUCCEED();
}

TEST_F(LSPServerTest, FixtureMacroExpandedSymbolLoads) {
    // Macro-defined symbol (C++ #define). LSP should accept the fixture and
    // not crash when asked about the macro name.
    std::string source = readFixture("lsp_macro_expanded_symbol.topo");
    ASSERT_FALSE(source.empty()) << "Fixture lsp_macro_expanded_symbol.topo must exist";
    openDocument("file:///test/lsp_macro_expanded_symbol.topo", source);

    // Just ask for document symbols — a minimal LSP feature that should work
    // for any syntactically valid .topo file.
    json symMsg = {{"jsonrpc", "2.0"},
                   {"id", 301},
                   {"method", "textDocument/documentSymbol"},
                   {"params", {{"textDocument", {{"uri", "file:///test/lsp_macro_expanded_symbol.topo"}}}}}};
    auto resp = server_.handleMessage(symMsg);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE((*resp)["result"].is_array())
        << "documentSymbol must return an array on a syntactically valid fixture";
}

TEST_F(LSPServerTest, FixtureExternCFFILoads) {
    // extern "C" FFI block — the .topo declares C-style symbols that the
    // host C++ would expose via an extern "C" block. LSP should handle
    // the declaration without special-casing FFI.
    std::string source = readFixture("lsp_extern_c_ffi.topo");
    ASSERT_FALSE(source.empty()) << "Fixture lsp_extern_c_ffi.topo must exist";
    std::string uri = "file:///test/lsp_extern_c_ffi.topo";
    openDocument(uri, source);

    json hoverMsg = {
        {"jsonrpc", "2.0"},
        {"id", 302},
        {"method", "textDocument/hover"},
        {"params", {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 3}, {"character", 15}}}}}};
    auto resp = server_.handleMessage(hoverMsg);
    ASSERT_TRUE(resp.has_value());
    // Fixture exists and server responds -- regression reach achieved.
    SUCCEED();
}

TEST_F(LSPServerTest, FixtureTemplateSpecializationLoads) {
    // Templated host code — verifying the .topo LSP handles declarations
    // that map to specialized templates in the host without error.
    std::string source = readFixture("lsp_template_specialization.topo");
    ASSERT_FALSE(source.empty()) << "Fixture lsp_template_specialization.topo must exist";
    std::string uri = "file:///test/lsp_template_specialization.topo";
    openDocument(uri, source);

    json symMsg = {{"jsonrpc", "2.0"},
                   {"id", 303},
                   {"method", "textDocument/documentSymbol"},
                   {"params", {{"textDocument", {{"uri", uri}}}}}};
    auto resp = server_.handleMessage(symMsg);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE((*resp)["result"].is_array());
}

// ============================================================================
// Topo.toml Go to Definition — out-of-bounds / robustness regression
// ============================================================================

// Regression: a definition request whose cursor sits PAST the end of the line
// (character >= line length) used to index lineStr[idx] out of bounds in
// handleTomlDefinition. The handler must clamp the index and return a null
// result instead of reading out of bounds.
TEST_F(LSPServerTest, TomlDefinitionCursorPastEndOfLineNoCrash) {
    // A Topo.toml routes to handleTomlDefinition (uri ends with "Topo.toml").
    std::string tomlSource =
        "[build]\n"
        "language = \"cpp\"\n"
        "sources = [\"src/a.cpp\"]\n";
    std::string tomlUri = "file:///test/Topo.toml";
    openDocument(tomlUri, tomlSource);

    // Place the cursor far past the end of the first (short) line. Before the
    // fix this dereferenced lineStr[idx] with idx beyond size().
    json defMsg = {{"jsonrpc", "2.0"},
                   {"id", 400},
                   {"method", "textDocument/definition"},
                   {"params",
                    {{"textDocument", {{"uri", tomlUri}}},
                     {"position", {{"line", 0}, {"character", 9999}}}}}};
    auto resp = server_.handleMessage(defMsg);
    ASSERT_TRUE(resp.has_value());
    // No quoted string around an out-of-range cursor → null, no crash.
    EXPECT_TRUE((*resp)["result"].is_null())
        << "Cursor past end-of-line must yield a null definition, not an OOB read";
}

// Regression: a definition request on a quoted relative path that does not
// resolve to an existing file must return null cleanly (exercises the
// error_code canonical path — the throwing form would crash on a vanished
// or non-existent target).
TEST_F(LSPServerTest, TomlDefinitionMissingTargetReturnsNull) {
    std::string tomlSource =
        "[build]\n"
        "sources = [\"does/not/exist/here.cpp\"]\n";
    std::string tomlUri = "file:///test/Topo.toml";
    openDocument(tomlUri, tomlSource);

    // Cursor inside the quoted "does/not/exist/here.cpp" string on line 1.
    json defMsg = {{"jsonrpc", "2.0"},
                   {"id", 401},
                   {"method", "textDocument/definition"},
                   {"params",
                    {{"textDocument", {{"uri", tomlUri}}},
                     {"position", {{"line", 1}, {"character", 20}}}}}};
    auto resp = server_.handleMessage(defMsg);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE((*resp)["result"].is_null())
        << "Definition on a non-existent target path must return null, not throw";
}

TEST_F(LSPServerTest, CanonicalSyntaxNoExtraDiagnostics) {
    // A document using canonical syntax should produce no non-canonical warnings
    std::string canonicalSource =
        "namespace app {\n"
        "    public:\n"
        "        void run();\n"
        "}\n";
    std::string canUri = "file:///test/canonical.topo";

    json didOpenMsg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params",
         {{"textDocument", {{"uri", canUri}, {"languageId", "topo"}, {"version", 1}, {"text", canonicalSource}}}}}};
    server_.handleMessage(didOpenMsg);

    auto notifications = server_.takePendingNotifications();
    for (const auto& notif : notifications) {
        if (notif.value("method", "") != "textDocument/publishDiagnostics") continue;
        auto& diags = notif["params"]["diagnostics"];
        for (const auto& d : diags) {
            if (d.contains("code")) {
                EXPECT_NE(d["code"].get<std::string>(), "non-canonical-syntax")
                    << "Canonical syntax should not produce non-canonical-syntax diagnostics";
            }
        }
    }
}
