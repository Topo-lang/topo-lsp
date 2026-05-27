// LspSyncNotificationsTest.cpp
//
// Protocol-level tests for the textDocument/did* synchronization notifications.
// Uses LspProtocolClient to spawn a real topo-lsp subprocess over a pipe and
// exercises the full JSON-RPC round trip for each scenario.
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

// A canonical test document. Uses a logic block so the server's analyzer
// populates call sites in addition to declarations.
const std::string kDocAB =
    "namespace engine {\n"
    "    public:\n"
    "        void foo();\n"
    "        void bar();\n"
    "}\n";

const std::string kDocReplaced =
    "namespace replaced {\n"
    "    public:\n"
    "        void brandNew();\n"
    "}\n";

// Convenience: document-symbol request for a URI.
json docSymbolParams(const std::string& uri) {
    return json{{"textDocument", {{"uri", uri}}}};
}

// Helper to collect all symbol names (flattened) from a documentSymbol result.
// The server returns a hierarchical DocumentSymbol[] with optional `children`.
void collectNames(const json& nodes, std::vector<std::string>& out) {
    if (!nodes.is_array()) return;
    for (const auto& n : nodes) {
        if (n.contains("name")) out.push_back(n["name"].get<std::string>());
        if (n.contains("children")) collectNames(n["children"], out);
    }
}

} // namespace

// Common test fixture: every test starts a fresh client, initializes, and
// signals `initialized`. Each test brings its own documents to keep state
// isolated.
class LspSyncNotifications : public ::testing::Test {
protected:
    void SetUp() override {
        client_ = std::make_unique<LspProtocolClient>(kLspBinary);
        auto init = client_->initialize(nullptr);
        ASSERT_TRUE(responseHasResult(init)) << "initialize failed during SetUp";
        client_->initialized();
    }

    void TearDown() override {
        client_.reset();
    }

    std::unique_ptr<LspProtocolClient> client_;
};

TEST_F(LspSyncNotifications, DidOpenMakesDocumentQueryable) {
    const std::string uri = "file:///tmp/protocol-sync-open.topo";
    client_->didOpen(uri, kDocAB);

    auto resp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(uri));
    ASSERT_TRUE(responseHasResult(resp));
    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.is_array()) << "documentSymbol must return an array";
    EXPECT_GT(result.size(), 0u) << "didOpen should add the document to analysisCache";

    std::vector<std::string> names;
    collectNames(result, names);
    bool foundFoo = false, foundBar = false;
    for (const auto& n : names) {
        if (n == "foo") foundFoo = true;
        if (n == "bar") foundBar = true;
    }
    EXPECT_TRUE(foundFoo);
    EXPECT_TRUE(foundBar);
}

TEST_F(LspSyncNotifications, DidChangeIncrementalAppliesEdit) {
    const std::string uri = "file:///tmp/protocol-sync-incremental.topo";
    client_->didOpen(uri, kDocAB);

    // Replace the identifier "foo" at line 2 columns 13..16 with "renamed".
    //   Line 0: "namespace engine {"
    //   Line 1: "    public:"
    //   Line 2: "        void foo();"
    //                         ^col 13..16
    client_->didChangeRange(uri, 2, 13, 2, 16, "renamed");

    auto resp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(uri));
    ASSERT_TRUE(responseHasResult(resp));
    std::vector<std::string> names;
    collectNames((*resp)["result"], names);

    bool foundRenamed = false, foundFooAgain = false;
    for (const auto& n : names) {
        if (n == "renamed") foundRenamed = true;
        if (n == "foo") foundFooAgain = true;
    }
    EXPECT_TRUE(foundRenamed) << "incremental edit should have replaced 'foo' with 'renamed'";
    EXPECT_FALSE(foundFooAgain) << "'foo' should no longer exist after the edit";
}

TEST_F(LspSyncNotifications, DidChangeFullReplacesDocument) {
    const std::string uri = "file:///tmp/protocol-sync-full.topo";
    client_->didOpen(uri, kDocAB);

    // Full replace: LSP spec allows a contentChange without `range` that
    // replaces the entire document.
    client_->didChangeFull(uri, kDocReplaced);

    auto resp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(uri));
    ASSERT_TRUE(responseHasResult(resp));
    std::vector<std::string> names;
    collectNames((*resp)["result"], names);

    bool foundReplaced = false, foundBrandNew = false, foundEngine = false;
    for (const auto& n : names) {
        if (n == "replaced") foundReplaced = true;
        if (n == "brandNew") foundBrandNew = true;
        if (n == "engine") foundEngine = true;
    }
    EXPECT_TRUE(foundReplaced) << "full-replace should swap 'engine' namespace for 'replaced'";
    EXPECT_TRUE(foundBrandNew) << "new member 'brandNew' should be visible after full replace";
    EXPECT_FALSE(foundEngine) << "old 'engine' namespace must no longer exist";
}

TEST_F(LspSyncNotifications, DidSaveHandledGracefully) {
    // topo-lsp has an explicit `textDocument/didSave` handler (no-op, see
    // LSPServer.cpp handleMessage). Verify the notification is accepted and
    // the server stays responsive to subsequent requests.
    const std::string uri = "file:///tmp/protocol-sync-save.topo";
    client_->didOpen(uri, kDocAB);

    json saveParams = {{"textDocument", {{"uri", uri}}}, {"text", kDocAB}};
    client_->sendNotification("textDocument/didSave", saveParams);

    // Subsequent request must still work.
    auto resp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(uri));
    ASSERT_TRUE(responseHasResult(resp));
    EXPECT_TRUE((*resp)["result"].is_array());
    EXPECT_TRUE(client_->isRunning());
}

TEST_F(LspSyncNotifications, DidCloseRemovesDocument) {
    const std::string uri = "file:///tmp/protocol-sync-close.topo";
    client_->didOpen(uri, kDocAB);

    // Confirm it was added.
    auto preResp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(uri));
    ASSERT_TRUE(responseHasResult(preResp));
    EXPECT_GT((*preResp)["result"].size(), 0u);

    client_->didClose(uri);

    // After close, the server clears the URI from its caches; documentSymbol
    // returns an empty array (not an error).
    auto postResp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(uri));
    ASSERT_TRUE(responseHasResult(postResp));
    ASSERT_TRUE((*postResp)["result"].is_array());
    EXPECT_EQ((*postResp)["result"].size(), 0u) << "didClose should drop the document from cache";
}

TEST_F(LspSyncNotifications, DidChangeOnUnopenedDocumentIsIgnored) {
    // Sending didChange for a URI that was never didOpen'd must not crash the
    // server. The spec technically forbids this, but robust servers ignore it
    // and topo-lsp's handleDidChange returns early when the document is missing.
    const std::string bogusUri = "file:///tmp/protocol-sync-never-opened.topo";
    client_->didChangeFull(bogusUri, kDocReplaced);

    // A follow-up request to a real document must still work.
    const std::string realUri = "file:///tmp/protocol-sync-after-ghost.topo";
    client_->didOpen(realUri, kDocAB);
    auto resp = client_->sendRequest("textDocument/documentSymbol", docSymbolParams(realUri));
    ASSERT_TRUE(responseHasResult(resp));
    EXPECT_GT((*resp)["result"].size(), 0u);
    EXPECT_TRUE(client_->isRunning());
}
