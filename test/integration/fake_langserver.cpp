// fake_langserver — a deliberately broken LSP/JSON-RPC server used by the
// *BridgeMalformedResponseHandled integration tests.
//
// The real language-server bridges (ClangdBridge / RustAnalyzerBridge /
// JdtBridge / PyrightBridge) all funnel through topo::lsp::LSPBridge, which
// spawns the configured executable and exchanges Content-Length-framed
// JSON-RPC over the child's stdio. Pointing a bridge's start(exePath, ...)
// at THIS binary exercises the bridge's real framing + parsing + init path
// against malformed input, instead of stubbing the bridge internals.
//
// The contract under test: when the server speaks garbage, the bridge must
// degrade gracefully — start() fails (or the bridge ends up unavailable),
// every query API returns empty/nullopt, and the process neither crashes
// nor hangs.
//
// Mode is selected via argv[1], or the TOPO_FAKE_LSP_MODE environment
// variable when no arg is given (the bridges spawn the server with their
// own fixed args, so the test selects the fault via the inherited env).
// Each mode reads the client's `initialize`
// request off stdin (so the handshake gets far enough to matter) and then
// injects one class of malformed framing/payload:
//
//   missing-field   valid frame, JSON-RPC response missing "result"/"error"
//   type-mismatch   valid frame, "id" is a string where an int is expected
//   bad-length      Content-Length header is non-numeric ("abc")
//   short-body      Content-Length larger than the body actually sent, then EOF
//   not-json        well-framed body that is not JSON at all
//   nonzero-exit    consume initialize, then exit(3) without responding
//
// Intentionally dependency-free (no JSON lib): every response is a fixed
// byte pattern so the harness cannot itself be the thing under test.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#endif

namespace {

// Read one Content-Length-framed message off stdin and discard it. Returns
// false on EOF before a full frame arrives. We only need to drain the
// client's `initialize` so the bridge has actually started talking before
// we inject the fault.
bool drainOneFrame() {
    std::string header;
    int contentLength = -1;
    // Read headers line by line until the blank line terminator.
    for (;;) {
        int c = std::getchar();
        if (c == EOF) return false;
        if (c == '\r') {
            int n = std::getchar();
            if (n == '\n') {
                if (header.empty()) break; // end of headers
                const char* prefix = "Content-Length:";
                if (header.rfind(prefix, 0) == 0) {
                    contentLength = std::atoi(header.c_str() + std::strlen(prefix));
                }
                header.clear();
                continue;
            }
            header.push_back(static_cast<char>(c));
            if (n != EOF) header.push_back(static_cast<char>(n));
        } else {
            header.push_back(static_cast<char>(c));
        }
    }
    if (contentLength <= 0) return false;
    for (int i = 0; i < contentLength; ++i) {
        if (std::getchar() == EOF) return false;
    }
    return true;
}

void writeRaw(const std::string& bytes) {
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

// Frame a body with a correct Content-Length header.
void writeFramed(const std::string& body) {
    writeRaw("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string mode = "missing-field";
    if (argc > 1) {
        mode = argv[1];
    } else if (const char* env = std::getenv("TOPO_FAKE_LSP_MODE")) {
        mode = env;
    }

    // Always let the client get its initialize request out first so the
    // bridge's handshake path is genuinely exercised.
    if (!drainOneFrame()) {
        return 0; // client never spoke; nothing to corrupt
    }

    if (mode == "missing-field") {
        // Well-framed JSON-RPC envelope with neither "result" nor "error".
        writeFramed(R"({"jsonrpc":"2.0","id":1})");
    } else if (mode == "type-mismatch") {
        // "id" is a string; LSPBridge expects an int and does
        // (*msg)["id"].get<int>() — must not take down the process.
        writeFramed(R"({"jsonrpc":"2.0","id":"not-an-int","result":{}})");
    } else if (mode == "bad-length") {
        // Non-numeric Content-Length. The bridge's header parser must not
        // throw an uncaught std::invalid_argument from std::stoi.
        writeRaw("Content-Length: abc\r\n\r\n{}");
    } else if (mode == "short-body") {
        // Promise 100 bytes, deliver 2, then EOF.
        writeRaw("Content-Length: 100\r\n\r\n{}");
    } else if (mode == "not-json") {
        writeFramed("this is definitely not json <<<>>>");
    } else if (mode == "nonzero-exit") {
        // Acknowledge nothing and die with a non-zero status.
        return 3;
    } else {
        // Unknown mode: behave like missing-field.
        writeFramed(R"({"jsonrpc":"2.0","id":1})");
    }

    // Hold the pipe open briefly so the bridge observes the malformed frame
    // (or the EOF for short-body) rather than racing our exit. Then leave;
    // closing stdout signals connection loss, which the bridge must survive.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}
