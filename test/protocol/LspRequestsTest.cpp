// LspRequestsTest.cpp
//
// Protocol-level tests for read-only request methods on top of an initialized
// and populated document state. Each test starts its own subprocess, does the
// initialize/didOpen handshake, then exercises one request endpoint.
#include "LspProtocolFixture.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace topo::lsp::test;
using json = nlohmann::json;

#ifndef TOPO_LSP_BINARY
#error "TOPO_LSP_BINARY must be defined to the absolute path of the topo-lsp executable"
#endif

namespace {

constexpr const char* kLspBinary = TOPO_LSP_BINARY;

// A document that exercises declarations AND a pipeline logic block so that
// the analyzer populates both function declarations and call-site references.
// Pipeline edge syntax (`A -> B;`) is what the references handler actually
// indexes — `stage<N> f()` form does not generate reference entries. See
// LSPServerTest.ReferencesForPipelineFunction for the canonical pattern.
//
//   Line  0: namespace engine {
//   Line  1:     public:
//   Line  2:         void fetch();
//   Line  3:         void decode();
//   Line  4:         void execute();
//   Line  5:         void run();
//   Line  6:
//   Line  7:         fn run {
//   Line  8:             fetch -> decode;
//   Line  9:             decode -> execute;
//   Line 10:             execute -> void;
//   Line 11:         }
//   Line 12: }
//
// "fetch" declaration header "void fetch" starts at (line=2, col=13).
// "fetch" call site in the pipeline starts at (line=8, col=12).
const std::string kDoc =
    "namespace engine {\n"
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

// A deliberately under-indented but still parseable document used by the
// formatting test. The formatter should rewrite whitespace to canonical form.
// We keep the structure simple (no extra spaces inside tokens) so the Lexer
// certainly tokenises it without diagnostics.
const std::string kMessyDoc =
    "namespace messy {\n"
    "public:\n"
    "void foo();\n"
    "void bar();\n"
    "}\n";

constexpr const char* kUri = "file:///tmp/protocol-requests.topo";
constexpr const char* kMessyUri = "file:///tmp/protocol-requests-messy.topo";

} // namespace

// Fixture: shared initialize+didOpen to keep each test focused on its
// endpoint.
class LspRequests : public ::testing::Test {
protected:
    void SetUp() override {
        client_ = std::make_unique<LspProtocolClient>(kLspBinary);
        auto init = client_->initialize(nullptr);
        ASSERT_TRUE(responseHasResult(init)) << "initialize failed in SetUp";
        client_->initialized();
        client_->didOpen(kUri, kDoc);
    }

    void TearDown() override {
        client_.reset();
    }

    // Shorthand to build a textDocument/position params object.
    static json positionParams(const std::string& uri, int line, int character) {
        return json{
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}}};
    }

    std::unique_ptr<LspProtocolClient> client_;
};

TEST_F(LspRequests, CompletionReturnsItemsIncludingKeywords) {
    // Completion is expected to return a CompletionList (LSP 3.17 style)
    // with isIncomplete + items. At an empty-prefix position we should see
    // the full keyword catalog plus any declared symbols.
    json params = positionParams(kUri, 6, 0);
    auto resp = client_->sendRequest("textDocument/completion", params);
    ASSERT_TRUE(responseHasResult(resp));

    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.contains("items"));
    const auto& items = result["items"];
    ASSERT_TRUE(items.is_array());
    EXPECT_GT(items.size(), 0u);

    bool hasNamespaceKw = false;
    bool hasPublicKw = false;
    bool hasDeclaredFn = false;
    for (const auto& item : items) {
        if (!item.contains("label")) continue;
        std::string label = item["label"].get<std::string>();
        if (label == "namespace") hasNamespaceKw = true;
        if (label == "public") hasPublicKw = true;
        if (label == "fetch" || label == "decode") hasDeclaredFn = true;
    }
    EXPECT_TRUE(hasNamespaceKw);
    EXPECT_TRUE(hasPublicKw);
    EXPECT_TRUE(hasDeclaredFn) << "completion should include declared functions from the opened doc";
}

TEST_F(LspRequests, HoverOnDeclaredFunctionReturnsContents) {
    // Hover over the "fetch" identifier inside its declaration line.
    json params = positionParams(kUri, 2, 14); // middle of the word "fetch"
    auto resp = client_->sendRequest("textDocument/hover", params);
    ASSERT_TRUE(responseHasResult(resp));
    const auto& result = (*resp)["result"];
    ASSERT_FALSE(result.is_null()) << "hover on a declared function must return non-null";
    ASSERT_TRUE(result.contains("contents"));

    // LSP allows contents to be MarkupContent {kind,value} or a plain string
    // or a MarkedString[]. topo-lsp returns a MarkupContent object.
    const auto& contents = result["contents"];
    std::string text;
    if (contents.is_string()) {
        text = contents.get<std::string>();
    } else if (contents.contains("value")) {
        text = contents["value"].get<std::string>();
    }
    EXPECT_NE(text.find("fetch"), std::string::npos) << "hover text should mention the function name";
}

TEST_F(LspRequests, DefinitionOnCallSiteReturnsLocation) {
    // Definition from the call site "fetch" inside the pipeline logic block.
    // Position: line 8 (the `fetch -> decode;` line), column 14 — middle of
    // "fetch" (12 spaces indent, "fetch" occupies cols 12-16).
    json params = positionParams(kUri, 8, 14);
    auto resp = client_->sendRequest("textDocument/definition", params);
    ASSERT_TRUE(responseHasResult(resp));
    const auto& result = (*resp)["result"];
    ASSERT_FALSE(result.is_null()) << "definition on a known call site must return a location";

    // LSP accepts Location, Location[], or LocationLink[]. topo-lsp returns a
    // single Location object for simple within-file jumps.
    json location;
    if (result.is_array()) {
        ASSERT_GT(result.size(), 0u);
        location = result[0];
    } else {
        location = result;
    }
    ASSERT_TRUE(location.contains("uri"));
    ASSERT_TRUE(location.contains("range"));
    const auto& range = location["range"];
    ASSERT_TRUE(range.contains("start"));
    ASSERT_TRUE(range["start"].contains("line"));
}

TEST_F(LspRequests, ReferencesReturnsCallSites) {
    // References on the "init" declaration should yield the one call site
    // inside the logic block.
    json params = positionParams(kUri, 2, 14);
    params["context"] = json{{"includeDeclaration", true}};
    auto resp = client_->sendRequest("textDocument/references", params);
    ASSERT_TRUE(responseHasResult(resp));
    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    EXPECT_GE(result.size(), 1u) << "references should find at least the logic-block call site";

    for (const auto& loc : result) {
        ASSERT_TRUE(loc.contains("uri"));
        ASSERT_TRUE(loc.contains("range"));
    }
}

TEST_F(LspRequests, DocumentSymbolReturnsNamespaceAndMembers) {
    auto resp = client_->sendRequest("textDocument/documentSymbol",
                                     json{{"textDocument", {{"uri", kUri}}}});
    ASSERT_TRUE(responseHasResult(resp));
    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    EXPECT_GT(result.size(), 0u);

    bool foundEngine = false;
    for (const auto& sym : result) {
        ASSERT_TRUE(sym.contains("name"));
        ASSERT_TRUE(sym.contains("kind"));
        ASSERT_TRUE(sym.contains("range"));
        if (sym["name"].get<std::string>() == "engine") {
            foundEngine = true;
            if (sym.contains("children")) {
                EXPECT_GT(sym["children"].size(), 0u) << "engine namespace should have child functions";
            }
        }
    }
    EXPECT_TRUE(foundEngine);
}

TEST_F(LspRequests, SemanticTokensFullReturnsQuintupleData) {
    auto resp = client_->sendRequest("textDocument/semanticTokens/full",
                                     json{{"textDocument", {{"uri", kUri}}}});
    ASSERT_TRUE(responseHasResult(resp));
    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.contains("data"));
    const auto& data = result["data"];
    ASSERT_TRUE(data.is_array());
    EXPECT_GT(data.size(), 0u) << "a valid .topo document should produce some tokens";
    // LSP spec: data encodes tokens as [deltaLine, deltaChar, length, type, modifiers]
    EXPECT_EQ(data.size() % 5u, 0u) << "semantic tokens data length must be a multiple of 5";

    // Every element must be a non-negative integer.
    for (const auto& v : data) {
        ASSERT_TRUE(v.is_number_integer());
        EXPECT_GE(v.get<int>(), 0);
    }
}

TEST_F(LspRequests, FormattingReturnsTextEdits) {
    // Open a deliberately messy document so the formatter has something to do.
    client_->didOpen(kMessyUri, kMessyDoc);

    json params = {
        {"textDocument", {{"uri", kMessyUri}}},
        {"options", {{"tabSize", 4}, {"insertSpaces", true}}}};
    auto resp = client_->sendRequest("textDocument/formatting", params);
    ASSERT_TRUE(responseHasResult(resp));

    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array());
    ASSERT_GE(result.size(), 1u) << "formatting a parseable messy document should produce at least one edit";

    const auto& edit = result[0];
    ASSERT_TRUE(edit.contains("range"));
    ASSERT_TRUE(edit.contains("newText"));
    EXPECT_FALSE(edit["newText"].get<std::string>().empty());
}
