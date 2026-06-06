// LspProtocolHandshakeTest.cpp
//
// Protocol-level (over-pipe) tests for the LSP lifecycle handshake:
//   initialize -> initialized -> ... -> shutdown -> exit
//
// Each test spawns a fresh topo-lsp subprocess via LspProtocolClient, which
// uses real JSON-RPC framing over stdin/stdout pipes. No mocks.
#include "LspProtocolFixture.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using namespace topo::lsp::test;
using json = nlohmann::json;

#ifndef TOPO_LSP_BINARY
#error "TOPO_LSP_BINARY must be defined to the absolute path of the topo-lsp executable"
#endif

namespace {
constexpr const char* kLspBinary = TOPO_LSP_BINARY;
}

TEST(LspProtocolHandshake, InitializeReturnsCapabilities) {
    LspProtocolClient client{kLspBinary};

    auto resp = client.initialize(nullptr);
    ASSERT_TRUE(resp.has_value()) << "initialize must return a response within 5s";
    ASSERT_TRUE(responseHasResult(resp)) << "initialize response must contain 'result', not 'error'";

    const auto& result = (*resp)["result"];
    ASSERT_TRUE(result.contains("capabilities"));
    const auto& caps = result["capabilities"];

    // textDocumentSync: Incremental (LSP kind = 2).
    ASSERT_TRUE(caps.contains("textDocumentSync"));
    const auto& sync = caps["textDocumentSync"];
    ASSERT_TRUE(sync.contains("change"));
    EXPECT_EQ(sync["change"].get<int>(), 2) << "server must advertise Incremental (2) text sync";
    // Must advertise save so clients know didSave is accepted.
    ASSERT_TRUE(sync.contains("save"));
    const auto& save = sync["save"];
    ASSERT_TRUE(save.contains("includeText"));

    ASSERT_TRUE(result.contains("serverInfo"));
    const auto& info = result["serverInfo"];
    ASSERT_TRUE(info.contains("name"));
    EXPECT_EQ(info["name"].get<std::string>(), "topo-lsp");
    ASSERT_TRUE(info.contains("version"));
    // Version is an informational string — just assert it is non-empty.
    EXPECT_FALSE(info["version"].get<std::string>().empty());
}

TEST(LspProtocolHandshake, InitializeIncludesAllDeclaredProviders) {
    // Regression test: the server must advertise every capability its
    // dispatcher implements. If any are missing, clients will route around them.
    LspProtocolClient client{kLspBinary};

    auto resp = client.initialize(nullptr);
    ASSERT_TRUE(responseHasResult(resp));
    const auto& caps = (*resp)["result"]["capabilities"];

    EXPECT_TRUE(caps.contains("hoverProvider"));
    EXPECT_TRUE(caps.contains("definitionProvider"));
    EXPECT_TRUE(caps.contains("referencesProvider"));
    EXPECT_TRUE(caps.contains("completionProvider"));
    EXPECT_TRUE(caps.contains("documentSymbolProvider"));
    EXPECT_TRUE(caps.contains("documentFormattingProvider"));
    EXPECT_TRUE(caps.contains("documentRangeFormattingProvider"));
    EXPECT_TRUE(caps.contains("codeActionProvider"));
    EXPECT_TRUE(caps.contains("semanticTokensProvider"));

    // completionProvider must declare trigger characters so clients know when
    // to request completions. ':' is the sole trigger in topo-lsp today.
    const auto& comp = caps["completionProvider"];
    ASSERT_TRUE(comp.contains("triggerCharacters"));
    ASSERT_TRUE(comp["triggerCharacters"].is_array());
    EXPECT_GT(comp["triggerCharacters"].size(), 0u);
}

TEST(LspProtocolHandshake, SemanticTokensLegendIsWellFormed) {
    // The semantic tokens legend is a fixed contract between server and
    // client: if either side's index list shifts, all highlighting breaks.
    LspProtocolClient client{kLspBinary};

    auto resp = client.initialize(nullptr);
    ASSERT_TRUE(responseHasResult(resp));
    const auto& caps = (*resp)["result"]["capabilities"];

    ASSERT_TRUE(caps.contains("semanticTokensProvider"));
    const auto& stp = caps["semanticTokensProvider"];
    ASSERT_TRUE(stp.contains("legend"));
    const auto& legend = stp["legend"];

    ASSERT_TRUE(legend.contains("tokenTypes"));
    const auto& types = legend["tokenTypes"];
    ASSERT_TRUE(types.is_array());
    EXPECT_GE(types.size(), 12u) << "expected at least 12 token types (see LSPServer::handleInitialize)";

    // Spot-check a couple of well-known entries so we catch accidental reordering.
    bool hasKeyword = false;
    bool hasFunction = false;
    for (const auto& t : types) {
        if (!t.is_string()) continue;
        const std::string name = t.get<std::string>();
        if (name == "keyword") hasKeyword = true;
        if (name == "function") hasFunction = true;
    }
    EXPECT_TRUE(hasKeyword);
    EXPECT_TRUE(hasFunction);

    ASSERT_TRUE(legend.contains("tokenModifiers"));
    const auto& mods = legend["tokenModifiers"];
    ASSERT_TRUE(mods.is_array());
    EXPECT_GE(mods.size(), 4u);

    ASSERT_TRUE(stp.contains("full"));
    EXPECT_TRUE(stp["full"].get<bool>());
}

TEST(LspProtocolHandshake, InitializedNotificationKeepsServerAlive) {
    // `initialized` is a notification, so it has no response. The server must
    // accept it without error and continue to serve subsequent requests.
    LspProtocolClient client{kLspBinary};

    auto init = client.initialize(nullptr);
    ASSERT_TRUE(responseHasResult(init));
    client.initialized();

    // Send a benign documentSymbol request to confirm the loop is still alive.
    // We do NOT require a non-empty result here — we only need the server to
    // respond within the timeout and to be well-formed (result field present).
    json params = {{"textDocument", {{"uri", "file:///tmp/protocol-handshake-alive.topo"}}}};
    auto resp = client.sendRequest("textDocument/documentSymbol", params);
    ASSERT_TRUE(resp.has_value()) << "server did not respond to documentSymbol after initialized";
    EXPECT_TRUE(responseHasResult(resp));
    EXPECT_TRUE(client.isRunning());
}

TEST(LspProtocolHandshake, ShutdownReturnsNullResult) {
    // Per LSP 3.17: `shutdown` must return `result: null`.
    LspProtocolClient client{kLspBinary};
    ASSERT_TRUE(responseHasResult(client.initialize(nullptr)));
    client.initialized();

    auto resp = client.shutdown();
    ASSERT_TRUE(resp.has_value()) << "shutdown must produce a response";
    ASSERT_TRUE(responseHasResult(resp));
    EXPECT_TRUE((*resp)["result"].is_null()) << "shutdown result must be JSON null";
    // Server loop should still be alive until `exit`.
    EXPECT_TRUE(client.isRunning());
}

TEST(LspProtocolHandshake, ExitAfterShutdownTerminatesProcess) {
    LspProtocolClient client{kLspBinary};
    ASSERT_TRUE(responseHasResult(client.initialize(nullptr)));
    client.initialized();
    ASSERT_TRUE(responseHasResult(client.shutdown()));

    client.exit();
    int code = client.waitForExit(Millis{3000});
    EXPECT_EQ(code, 0) << "exit after shutdown must yield exit code 0 per LSP 3.17 §3.17";
}

TEST(LspProtocolHandshake, ExitWithoutShutdownReturnsNonZero) {
    // LSP 3.17 §3.17: `exit` without a prior `shutdown` is a protocol error;
    // the server must exit with a non-zero code.
    LspProtocolClient client{kLspBinary};
    ASSERT_TRUE(responseHasResult(client.initialize(nullptr)));
    client.initialized();

    client.exit();
    int code = client.waitForExit(Millis{3000});
    EXPECT_NE(code, 0) << "exit without prior shutdown must yield non-zero exit code";
}

TEST(LspProtocolHandshake, RequestBeforeInitializeReturnsServerNotInitialized) {
    // LSP 3.17 §3.16: any request arriving before the `initialized`
    // notification must be answered with -32002 ServerNotInitialized.
    LspProtocolClient client{kLspBinary};

    json params = {{"textDocument", {{"uri", "file:///tmp/preinit.topo"}}},
                   {"position", {{"line", 0}, {"character", 0}}}};
    auto resp = client.sendRequest("textDocument/hover", params);
    ASSERT_TRUE(resp.has_value()) << "server must respond (with error) before initialize";
    ASSERT_TRUE(resp->contains("error")) << "pre-initialize request must return an error, not a result";
    const auto& err = (*resp)["error"];
    ASSERT_TRUE(err.contains("code"));
    EXPECT_EQ(err["code"].get<int>(), -32002) << "error code must be ServerNotInitialized (-32002)";

    // We never sent `initialize`, so the client destructor's shutdown will
    // be rejected (correctly) and its follow-up `exit` will terminate the
    // server with non-zero status — both are acceptable for cleanup.
}

TEST(LspProtocolHandshake, MalformedRequestDoesNotCrashServer) {
    // Robustness regression: a parseable-but-malformed request (here a hover
    // whose `textDocument.uri` is a number rather than a string) makes a
    // handler throw nlohmann::json::type_error. The main loop's exception
    // barrier must convert that throw into a JSON-RPC error (-32602) for the
    // request instead of letting it unwind past main() and std::terminate the
    // server for every open document. After the bad frame, the server must
    // still answer a subsequent valid request.
    LspProtocolClient client{kLspBinary};
    ASSERT_TRUE(responseHasResult(client.initialize(nullptr)));
    client.initialized();

    // uri is an integer, not a string -> get<std::string>() throws type_error.
    json badParams = {{"textDocument", {{"uri", 12345}}},
                      {"position", {{"line", 0}, {"character", 0}}}};
    auto badResp = client.sendRequest("textDocument/hover", badParams);
    ASSERT_TRUE(badResp.has_value()) << "server must respond to a malformed request, not crash";
    ASSERT_TRUE(badResp->contains("error")) << "malformed request must return an error object";
    EXPECT_EQ((*badResp)["error"]["code"].get<int>(), -32602)
        << "malformed params must map to InvalidParams (-32602)";

    // The server must still be alive and serving after the bad frame.
    EXPECT_TRUE(client.isRunning()) << "server must survive a malformed request";
    json okParams = {{"textDocument", {{"uri", "file:///tmp/protocol-malformed-alive.topo"}}}};
    auto okResp = client.sendRequest("textDocument/documentSymbol", okParams);
    ASSERT_TRUE(okResp.has_value()) << "server did not respond after a malformed request";
    EXPECT_TRUE(responseHasResult(okResp));
}

TEST(LspProtocolHandshake, UnknownRequestReturnsMethodNotFound) {
    // Per JSON-RPC 2.0, an unknown method on a request (has id) must produce
    // error code -32601 (Method not found).
    LspProtocolClient client{kLspBinary};
    ASSERT_TRUE(responseHasResult(client.initialize(nullptr)));
    client.initialized();

    auto resp = client.sendRequest("textDocument/bogusMethod", json::object());
    ASSERT_TRUE(resp.has_value());
    ASSERT_TRUE(resp->contains("error")) << "unknown method must return an error object, not a result";
    const auto& err = (*resp)["error"];
    ASSERT_TRUE(err.contains("code"));
    EXPECT_EQ(err["code"].get<int>(), -32601) << "error code must match JSON-RPC MethodNotFound";
    ASSERT_TRUE(err.contains("message"));
    EXPECT_FALSE(err["message"].get<std::string>().empty());
}
