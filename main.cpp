#include "LSPServer.h"

// Per-language plugin headers are conditionally included based on which
// upstream topo-lang-<lang> packages were found at configure time. Each
// TOPO_LSP_WITH_<LANG>_PLUGIN macro is defined by CMake when the matching
// topo::lang-<lang>::Topo<Lang>Plugin imported target is available.
#ifdef TOPO_LSP_WITH_CPP_PLUGIN
#include "CppPlugin.h"
#endif
#ifdef TOPO_LSP_WITH_RUST_PLUGIN
#include "RustPlugin.h"
#endif
#ifdef TOPO_LSP_WITH_JAVA_PLUGIN
#include "JavaPlugin.h"
#endif
#ifdef TOPO_LSP_WITH_PYTHON_PLUGIN
#include "PythonPlugin.h"
#endif
#ifdef TOPO_LSP_WITH_TYPESCRIPT_PLUGIN
#include "TypeScriptPlugin.h"
#endif

#include <iostream>
#include <optional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using json = nlohmann::json;

// Read a JSON-RPC message from stdin following the LSP base protocol:
// Content-Length: N\r\n\r\n<N bytes of JSON>
static std::optional<json> readMessage() {
    // Read headers
    std::string header;
    int contentLength = -1;

    while (true) {
        std::string line;
        // Read until \r\n
        while (true) {
            int ch = std::cin.get();
            if (ch == EOF || !std::cin.good()) return std::nullopt;
            if (ch == '\r') {
                int next = std::cin.get();
                if (next == '\n') break;
                line += static_cast<char>(ch);
                if (next != EOF) line += static_cast<char>(next);
            } else {
                line += static_cast<char>(ch);
            }
        }

        if (line.empty()) break; // Empty line = end of headers

        // Parse Content-Length header
        const std::string prefix = "Content-Length: ";
        if (line.substr(0, prefix.size()) == prefix) {
            contentLength = std::stoi(line.substr(prefix.size()));
        }
    }

    if (contentLength <= 0) return std::nullopt;

    // Read body
    std::string body(contentLength, '\0');
    std::cin.read(&body[0], contentLength);
    if (!std::cin.good()) return std::nullopt;

    try {
        return json::parse(body);
    } catch (const json::parse_error& e) {
        std::cerr << "[topo-lsp] JSON parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

// Write a JSON-RPC message to stdout.
static void writeMessage(const json& msg) {
    std::string body = msg.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

int main() {
#ifdef _WIN32
    // Set stdin/stdout to binary mode on Windows to prevent \n -> \r\n translation
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

#ifdef TOPO_LSP_WITH_CPP_PLUGIN
    topo::lang::registerCppPlugin();
#endif
#ifdef TOPO_LSP_WITH_RUST_PLUGIN
    topo::lang::registerRustPlugin();
#endif
#ifdef TOPO_LSP_WITH_JAVA_PLUGIN
    topo::lang::registerJavaPlugin();
#endif
#ifdef TOPO_LSP_WITH_PYTHON_PLUGIN
    topo::lang::registerPythonPlugin();
#endif
#ifdef TOPO_LSP_WITH_TYPESCRIPT_PLUGIN
    topo::lang::registerTypeScriptPlugin();
#endif

    std::cerr << "[topo-lsp] Starting Topo Language Server...\n";

    topo::lsp::LSPServer server;

    while (!server.shouldExit()) {
        auto msg = readMessage();
        if (!msg) {
            std::cerr << "[topo-lsp] Failed to read message, exiting.\n";
            break;
        }

        std::cerr << "[topo-lsp] << " << msg->value("method", "(response)") << "\n";

        auto response = server.handleMessage(*msg);

        // Send any pending notifications (e.g. publishDiagnostics)
        for (auto& notification : server.takePendingNotifications()) {
            writeMessage(notification);
        }

        // Send the response if there is one
        if (response) {
            writeMessage(*response);
        }
    }

    std::cerr << "[topo-lsp] Server exiting.\n";
    return server.exitCode();
}
