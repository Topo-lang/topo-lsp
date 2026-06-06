#include "LSPServer.h"
#include "Formatter.h"

#include "topo/Lang/LanguagePlugin.h"
#include "topo/Build/ConfigValidator.h"
#include "topo/Format/Formatter.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Platform/FileGlob.h"
#include "topo/Sema/ImportResolver.h"
#include "topo/Sema/SemanticAnalyzer.h"
#include "topo/Stdlib/Types.h"
#include "topo/Transform/TransformRecord.h"

#include <toml++/toml.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stack>

namespace fs = std::filesystem;

namespace topo::lsp {

// ============================================================================
// HostFunctionIndex implementation
// ============================================================================

void HostFunctionIndex::scanFile(const std::string& filepath) try {
    fs::path p(filepath);
    auto ext = p.extension().string();
    if (ext == ".rs") {
        scanRustFile(filepath);
    } else if (ext == ".java") {
        scanJavaFile(filepath);
    } else if (ext == ".py") {
        scanPythonFile(filepath);
    } else {
        scanCppFile(filepath);
    }
} catch (const std::exception& e) {
    std::cerr << "[topo-lsp] Warning: failed to scan " << filepath << ": " << e.what() << "\n";
} catch (...) {
    std::cerr << "[topo-lsp] Warning: failed to scan " << filepath << "\n";
}

void HostFunctionIndex::scanCppFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    // C++ keywords that look like function definitions but aren't
    static const std::vector<std::string> cppKeywords = {
        "if",     "for",      "while",    "switch",        "return",   "class", "struct", "enum",   "typedef",
        "using",  "template", "catch",    "else",          "do",       "try",   "throw",  "delete", "new",
        "sizeof", "alignof",  "decltype", "static_assert", "namespace"};

    std::stack<std::string> nsStack;
    std::string line;
    int lineNum = 0;
    int braceDepth = 0;
    // Track brace depth at which each namespace was entered
    std::stack<int> nsDepths;

    // Regex for function definition: capture the identifier immediately before '('
    // Avoids catastrophic backtracking by not mixing spaces into character classes.
    std::regex funcDefRegex(R"(\b(\w+)\s*\()");
    // Regex for namespace: namespace X {
    std::regex nsRegex(R"(^\s*namespace\s+([\w:]+)\s*\{)");

    while (std::getline(file, line)) {
        ++lineNum;

        // Track braces for namespace scope
        for (char c : line) {
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}') {
                --braceDepth;
                // Pop namespace if we're back to its entry depth
                if (!nsDepths.empty() && braceDepth == nsDepths.top()) {
                    nsStack.pop();
                    nsDepths.pop();
                }
            }
        }

        // Check for namespace declaration
        std::smatch nsMatch;
        if (std::regex_search(line, nsMatch, nsRegex)) {
            std::string nsName = nsMatch[1].str();
            nsStack.push(nsName);
            nsDepths.push(braceDepth - 1);
            continue;
        }

        // Check for function definition
        std::smatch funcMatch;
        if (std::regex_search(line, funcMatch, funcDefRegex)) {
            std::string funcName = funcMatch[1].str();

            // Skip C++ keywords
            bool isKeyword = false;
            for (const auto& kw : cppKeywords) {
                if (funcName == kw) {
                    isKeyword = true;
                    break;
                }
            }
            if (isKeyword) continue;

            // Build qualified name from namespace stack
            std::string qualified;
            {
                std::vector<std::string> nsParts;
                std::stack<std::string> tmp = nsStack;
                while (!tmp.empty()) {
                    nsParts.push_back(tmp.top());
                    tmp.pop();
                }
                std::reverse(nsParts.begin(), nsParts.end());
                for (const auto& ns : nsParts) {
                    if (!qualified.empty()) qualified += "::";
                    qualified += ns;
                }
            }
            if (!qualified.empty()) qualified += "::";
            qualified += funcName;

            if (index_.find(qualified) == index_.end()) index_[qualified] = HostSymbolLocation{filepath, lineNum};
            if (index_.find(funcName) == index_.end()) index_[funcName] = HostSymbolLocation{filepath, lineNum};
        }
    }
}

void HostFunctionIndex::scanRustFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    // Rust keywords that look like function definitions but aren't
    static const std::vector<std::string> rustKeywords = {
        "if",   "for",  "while", "match", "return", "let",    "mut",    "use",   "mod",   "struct",
        "enum", "impl", "trait", "pub",   "crate",  "super",  "self",   "where", "async", "await",
        "move", "loop", "fn",    "type",  "const",  "static", "unsafe", "extern"};

    // Regex for Rust function definitions
    std::regex funcDefRegex(R"(^\s*(?:pub(?:\([\w]+\))?\s+)?(?:async\s+)?(?:unsafe\s+)?fn\s+(\w+)\s*[\(<])");
    // Regex for Rust module definitions
    std::regex modRegex(R"(^\s*(?:pub\s+)?mod\s+(\w+)\s*\{)");

    std::stack<std::string> modStack;
    std::stack<int> modDepths;
    std::string line;
    int lineNum = 0;
    int braceDepth = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // Track braces for module scope
        for (char c : line) {
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}') {
                --braceDepth;
                if (!modDepths.empty() && braceDepth == modDepths.top()) {
                    modStack.pop();
                    modDepths.pop();
                }
            }
        }

        // Check for module declaration
        std::smatch modMatch;
        if (std::regex_search(line, modMatch, modRegex)) {
            std::string modName = modMatch[1].str();
            modStack.push(modName);
            modDepths.push(braceDepth - 1);
            continue;
        }

        // Check for function definition
        std::smatch funcMatch;
        if (std::regex_search(line, funcMatch, funcDefRegex)) {
            std::string funcName = funcMatch[1].str();

            // Skip Rust keywords
            bool isKeyword = false;
            for (const auto& kw : rustKeywords) {
                if (funcName == kw) {
                    isKeyword = true;
                    break;
                }
            }
            if (isKeyword) continue;

            // Build qualified name from module stack
            std::string qualified;
            {
                std::vector<std::string> modParts;
                std::stack<std::string> tmp = modStack;
                while (!tmp.empty()) {
                    modParts.push_back(tmp.top());
                    tmp.pop();
                }
                std::reverse(modParts.begin(), modParts.end());
                for (const auto& m : modParts) {
                    if (!qualified.empty()) qualified += "::";
                    qualified += m;
                }
            }
            if (!qualified.empty()) qualified += "::";
            qualified += funcName;

            if (index_.find(qualified) == index_.end()) index_[qualified] = HostSymbolLocation{filepath, lineNum};
            if (index_.find(funcName) == index_.end()) index_[funcName] = HostSymbolLocation{filepath, lineNum};
        }
    }
}

void HostFunctionIndex::scanJavaFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    // Java keywords that look like method definitions but aren't
    static const std::vector<std::string> javaKeywords = {"if",
                                                          "for",
                                                          "while",
                                                          "switch",
                                                          "return",
                                                          "class",
                                                          "interface",
                                                          "enum",
                                                          "import",
                                                          "package",
                                                          "new",
                                                          "throw",
                                                          "catch",
                                                          "try",
                                                          "synchronized",
                                                          "instanceof",
                                                          "assert",
                                                          "super",
                                                          "this"};

    // Regex for Java method definitions: capture the identifier before '('
    // Avoids catastrophic backtracking from overlapping \s in character classes.
    std::regex funcDefRegex(R"(\b(\w+)\s*\()");
    // Regex for class/interface declarations (to build qualified scope)
    std::regex classRegex(
        R"(^\s*(?:(?:public|private|protected|static|abstract|final)\s+)*(?:class|interface|enum)\s+(\w+))");

    std::stack<std::string> scopeStack;
    std::stack<int> scopeDepths;
    std::string line;
    int lineNum = 0;
    int braceDepth = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        for (char c : line) {
            if (c == '{')
                ++braceDepth;
            else if (c == '}') {
                --braceDepth;
                if (!scopeDepths.empty() && braceDepth == scopeDepths.top()) {
                    scopeStack.pop();
                    scopeDepths.pop();
                }
            }
        }

        // Check for class/interface declaration — track scope only
        std::smatch classMatch;
        if (std::regex_search(line, classMatch, classRegex)) {
            std::string className = classMatch[1].str();
            scopeStack.push(className);
            scopeDepths.push(braceDepth - 1);
            continue;
        }

        // Check for method definition
        std::smatch funcMatch;
        if (std::regex_search(line, funcMatch, funcDefRegex)) {
            std::string funcName = funcMatch[1].str();

            bool isKeyword = false;
            for (const auto& kw : javaKeywords) {
                if (funcName == kw) {
                    isKeyword = true;
                    break;
                }
            }
            if (isKeyword) continue;

            // Build qualified name from scope stack
            std::string qualified;
            {
                std::vector<std::string> parts;
                std::stack<std::string> tmp = scopeStack;
                while (!tmp.empty()) {
                    parts.push_back(tmp.top());
                    tmp.pop();
                }
                std::reverse(parts.begin(), parts.end());
                for (const auto& p : parts) {
                    if (!qualified.empty()) qualified += ".";
                    qualified += p;
                }
            }
            if (!qualified.empty()) qualified += ".";
            qualified += funcName;

            if (index_.find(qualified) == index_.end()) index_[qualified] = HostSymbolLocation{filepath, lineNum};
            if (index_.find(funcName) == index_.end()) index_[funcName] = HostSymbolLocation{filepath, lineNum};
        }
    }
}

void HostFunctionIndex::scanPythonFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    static const std::regex funcRe(R"(^\s*(?:async\s+)?def\s+(\w+))");
    static const std::regex classRe(R"(^\s*class\s+(\w+))");

    std::stack<std::pair<int, std::string>> scopeStack; // (indent, name)
    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;
        if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) continue;

        int indent = 0;
        for (char c : line) {
            if (c == ' ')
                ++indent;
            else if (c == '\t')
                indent += 4;
            else
                break;
        }

        // Pop scopes that ended
        while (!scopeStack.empty() && scopeStack.top().first >= indent)
            scopeStack.pop();

        std::smatch match;
        if (std::regex_search(line, match, classRe)) {
            std::string className = match[1].str();
            scopeStack.push({indent, className});
            continue;
        }

        if (std::regex_search(line, match, funcRe)) {
            std::string funcName = match[1].str();

            // Build qualified name using dot separator (Python convention)
            std::string qualified;
            std::stack<std::pair<int, std::string>> tmp = scopeStack;
            std::vector<std::string> parts;
            while (!tmp.empty()) {
                parts.push_back(tmp.top().second);
                tmp.pop();
            }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                if (!qualified.empty()) qualified += ".";
                qualified += *it;
            }
            if (!qualified.empty()) qualified += ".";
            qualified += funcName;

            if (index_.find(qualified) == index_.end()) index_[qualified] = HostSymbolLocation{filepath, lineNum};
            if (index_.find(funcName) == index_.end()) index_[funcName] = HostSymbolLocation{filepath, lineNum};

            scopeStack.push({indent, funcName});
        }
    }
}

void HostFunctionIndex::scanCompileCommands(const std::string& buildDir) {
    fs::path ccPath = fs::path(buildDir) / "compile_commands.json";
    if (!fs::exists(ccPath)) return;

    std::ifstream file(ccPath);
    if (!file.is_open()) return;

    try {
        json commands = json::parse(file);
        for (const auto& entry : commands) {
            // A parseable compile_commands.json whose `file` field is non-string
            // (or whose entry is non-object) must be skipped, not throw: guard
            // with is_string() before get<>() so one bad entry cannot abort the
            // scan or escape as json::type_error.
            if (entry.is_object() && entry.contains("file") && entry["file"].is_string()) {
                std::string srcFile = entry["file"].get<std::string>();
                // Only scan recognized source files
                fs::path p(srcFile);
                auto ext = p.extension().string();
                if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" || ext == ".rs" || ext == ".java") {
                    scanFile(srcFile);
                }
            }
        }
    } catch (const json::exception&) {
        // Silently ignore malformed compile_commands.json (syntactic parse
        // errors and any residual type errors).
    }
}

const HostSymbolLocation* HostFunctionIndex::find(const std::string& qualifiedName) const {
    auto it = index_.find(qualifiedName);
    if (it != index_.end()) return &it->second;
    return nullptr;
}

// ============================================================================
// Message dispatch
// ============================================================================

std::optional<json> LSPServer::handleMessage(const json& msg) {
    std::string method = msg.value("method", "");
    auto id = msg.contains("id") ? msg["id"] : json{};
    auto params = msg.value("params", json::object());

    // LSP 3.17 §3.16: reject requests before `initialized` notification with
    // -32002 ServerNotInitialized. A small whitelist lets lifecycle messages
    // through; anything else is rejected (requests → error, notifications
    // → silently dropped per spec).
    if (!initialized_) {
        const bool isLifecycle =
            method == "initialize" || method == "initialized" || method == "exit" ||
            method == "$/cancelRequest";
        if (!isLifecycle) {
            if (!id.is_null()) {
                return json{{"jsonrpc", "2.0"},
                            {"id", id},
                            {"error", {{"code", -32002}, {"message", "ServerNotInitialized"}}}};
            }
            return std::nullopt;
        }
    }

    // Requests (have "id")
    if (method == "initialize") return handleInitialize(id, params);
    if (method == "shutdown") return handleShutdown(id);
    if (method == "textDocument/hover") return handleHover(id, params);
    if (method == "textDocument/definition") return handleDefinition(id, params);
    if (method == "textDocument/references") return handleReferences(id, params);
    if (method == "textDocument/completion") return handleCompletion(id, params);
    if (method == "textDocument/documentSymbol") return handleDocumentSymbol(id, params);
    if (method == "textDocument/formatting") return handleFormatting(id, params);
    if (method == "textDocument/rangeFormatting") return handleRangeFormatting(id, params);
    if (method == "textDocument/codeAction") return handleCodeAction(id, params);
    if (method == "textDocument/semanticTokens/full") return handleSemanticTokensFull(id, params);

    // Notifications (no "id")
    if (method == "initialized") {
        initialized_ = true;
        return std::nullopt;
    }
    if (method == "exit") {
        handleExit();
        return std::nullopt;
    }
    if (method == "textDocument/didOpen") {
        handleDidOpen(params);
        return std::nullopt;
    }
    if (method == "textDocument/didChange") {
        handleDidChange(params);
        return std::nullopt;
    }
    if (method == "textDocument/didClose") {
        handleDidClose(params);
        return std::nullopt;
    }
    if (method == "textDocument/didSave") {
        // LSP 3.17 §3.17.2 — acknowledge save notification. topo-lsp keeps
        // document state in memory and uses didChange for sync, so no action
        // is required beyond not-dropping the message silently.
        return std::nullopt;
    }

    // Unknown method: if it has an id, respond with MethodNotFound
    if (!id.is_null()) {
        return json{
            {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}};
    }
    return std::nullopt;
}

std::vector<json> LSPServer::takePendingNotifications() {
    std::vector<json> result;
    result.swap(pendingNotifications_);
    return result;
}

// ============================================================================
// Lifecycle
// ============================================================================

json LSPServer::handleInitialize(const json& id, const json& params) {
    // Extract workspace root from rootUri or rootPath
    if (params.contains("rootUri") && !params["rootUri"].is_null()) {
        workspaceRoot_ = uriToPath(params["rootUri"].get<std::string>());
    } else if (params.contains("rootPath") && !params["rootPath"].is_null()) {
        workspaceRoot_ = params["rootPath"].get<std::string>();
    }

    // Load project config and build host function index
    if (!workspaceRoot_.empty()) {
        loadProjectConfig(workspaceRoot_);
    }

    // Start language-specific backend via plugin
    if (!workspaceRoot_.empty()) {
        std::string rootUri = pathToUri(workspaceRoot_);

        if (auto* plugin = lang::getPlugin(projectConfig_.language)) {
            bridge_ = plugin->createLSPBridge();
            if (bridge_) {
                bridge_->start(rootUri);
            }
        }

        // Principle 16 (no-silent-degradation): if the host LSP bridge is
        // missing or did not come up, emit ONE visible warning naming the
        // host LSP and stating host-language features are disabled. This
        // matches the topo-check L2-fallback non-silent behavior; emitted
        // once here, not per-feature-site.
        if (!bridge_ || !bridge_->isAvailable()) {
            const char* hostLsp = "the host language server";
            switch (projectConfig_.language) {
                case HostLanguage::Cpp:        hostLsp = "clangd"; break;
                case HostLanguage::Rust:       hostLsp = "rust-analyzer"; break;
                case HostLanguage::Java:       hostLsp = "jdtls"; break;
                case HostLanguage::Python:     hostLsp = "pyright"; break;
                case HostLanguage::TypeScript: hostLsp = "tsserver"; break;
                case HostLanguage::Mixed:      hostLsp = "the host language server"; break;
            }
            std::string warning =
                std::string("topo-lsp: host LSP '") + hostLsp +
                "' is unavailable; host-language features (hover signatures, "
                "go-to-definition, find-references in host code) are disabled. "
                ".topo declaration features remain active.";
            // Visible in the LSP client message log.
            pendingNotifications_.push_back(
                json{{"jsonrpc", "2.0"},
                     {"method", "window/showMessage"},
                     {"params", {{"type", 2 /* Warning */}, {"message", warning}}}});
            // Visible on stderr for headless / CI / log inspection.
            std::cerr << warning << std::endl;
        }
    }

    json capabilities = {{"textDocumentSync",
                          {
                              {"openClose", true},
                              {"change", 2}, // Incremental sync
                              {"save", {{"includeText", false}}},
                          }},
                         {"hoverProvider", true},
                         {"definitionProvider", true},
                         {"referencesProvider", true},
                         {"completionProvider", {{"triggerCharacters", json::array({":"})}}},
                         {"documentSymbolProvider", true},
                         {"documentFormattingProvider", true},
                         {"documentRangeFormattingProvider", true},
                         {"codeActionProvider", {{"codeActionKinds", json::array({"quickfix"})}}},
                         {"semanticTokensProvider",
                          {{"legend",
                            {{"tokenTypes",
                              json::array({
                                  "namespace",     // 0
                                  "type",          // 1
                                  "class",         // 2
                                  "function",      // 3
                                  "variable",      // 4
                                  "keyword",       // 5
                                  "comment",       // 6
                                  "string",        // 7
                                  "number",        // 8
                                  "operator",      // 9
                                  "typeParameter", // 10
                                  "property"       // 11
                              })},
                             {"tokenModifiers",
                              json::array({
                                  "declaration",    // 0
                                  "definition",     // 1
                                  "readonly",       // 2
                                  "static",         // 3
                                  "defaultLibrary"  // 4 — marks stdlib bridging types
                              })}}},
                           {"full", true}}}};

    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {{"capabilities", capabilities}, {"serverInfo", {{"name", "topo-lsp"}, {"version", "0.2.0"}}}}}};
}

json LSPServer::handleShutdown(const json& id) {
    shutdownRequested_ = true;
    if (bridge_) {
        bridge_->stop();
        bridge_.reset();
    }
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
}

void LSPServer::handleExit() {
    exitRequested_ = true;
}

// ============================================================================
// Project config & host function index
// ============================================================================

void LSPServer::loadProjectConfig(const std::string& rootPath) {
    fs::path tomlPath = fs::path(rootPath) / "Topo.toml";
    if (!fs::exists(tomlPath)) {
        std::cerr << "[topo-lsp] No Topo.toml found at " << tomlPath.string() << "\n";
        // Still try compile_commands.json
        fs::path buildDir = fs::path(rootPath) / "build";
        if (fs::exists(buildDir)) {
            hostIndex_.scanCompileCommands(buildDir.string());
        }
        return;
    }

    std::cerr << "[topo-lsp] Loading " << tomlPath.string() << "\n";

    toml::table tbl;
    try {
        tbl = toml::parse_file(tomlPath.string());
    } catch (const toml::parse_error& err) {
        std::cerr << "[topo-lsp] Failed to parse Topo.toml: " << err.what() << "\n";
        return;
    }
    std::string baseDir = tomlPath.parent_path().string();

    // [topo].root
    if (auto root = tbl["topo"]["root"].value<std::string>()) {
        projectConfig_.topoRoot = (fs::path(baseDir) / *root).string();
    }

    // [build].language
    if (auto lang = tbl["build"]["language"].value<std::string>()) {
        projectConfig_.language = parseHostLanguage(*lang);
    }

    // [build].sources — array of glob patterns
    if (auto* sources = tbl["build"]["sources"].as_array()) {
        for (const auto& elem : *sources) {
            if (auto pat = elem.value<std::string>()) {
                auto expanded = platform::globExpand(baseDir, *pat);
                projectConfig_.sources.insert(projectConfig_.sources.end(), expanded.begin(), expanded.end());
            }
        }
    }

    // [build].include
    if (auto* includes = tbl["build"]["include"].as_array()) {
        for (const auto& elem : *includes) {
            if (auto dir = elem.value<std::string>()) {
                projectConfig_.includeDirs.push_back((fs::path(baseDir) / *dir).string());
            }
        }
    }

    // Scan host sources from Topo.toml
    for (const auto& src : projectConfig_.sources) {
        hostIndex_.scanFile(src);
    }

    // For Rust projects, also scan src/ directory for .rs files
    if (projectConfig_.language == HostLanguage::Rust) {
        fs::path srcDir = fs::path(rootPath) / "src";
        if (fs::exists(srcDir) && fs::is_directory(srcDir)) {
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(srcDir, ec)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == ".rs") {
                    hostIndex_.scanFile(entry.path().string());
                }
            }
        }
    }

    // For Java projects, scan src/main/java and src/ for .java files
    if (projectConfig_.language == HostLanguage::Java) {
        // Standard Maven/Gradle layout
        fs::path mavenSrc = fs::path(rootPath) / "src" / "main" / "java";
        if (fs::exists(mavenSrc) && fs::is_directory(mavenSrc)) {
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(mavenSrc, ec)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == ".java") {
                    hostIndex_.scanFile(entry.path().string());
                }
            }
        }
        // Also check flat src/ directory
        fs::path flatSrc = fs::path(rootPath) / "src";
        if (fs::exists(flatSrc) && fs::is_directory(flatSrc)) {
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(flatSrc, ec)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == ".java") {
                    hostIndex_.scanFile(entry.path().string());
                }
            }
        }
    }

    // Also scan compile_commands.json from build/ directory
    fs::path buildDir = fs::path(rootPath) / "build";
    if (fs::exists(buildDir)) {
        hostIndex_.scanCompileCommands(buildDir.string());
    }

    if (!hostIndex_.empty()) {
        std::cerr << "[topo-lsp] Host function index loaded.\n";
    }
}

void LSPServer::discoverProjectConfig(const std::string& filepath) {
    // Walk up from the file's directory looking for Topo.toml
    fs::path dir = fs::path(filepath).parent_path();
    while (!dir.empty() && dir.has_parent_path()) {
        fs::path candidate = dir / "Topo.toml";
        if (fs::exists(candidate)) {
            std::cerr << "[topo-lsp] Discovered " << candidate.string() << "\n";
            loadProjectConfig(dir.string());
            return;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) break; // filesystem root
        dir = parent;
    }
}

// ============================================================================
// Document synchronization
// ============================================================================

static bool isTopoFile(const std::string& uri) {
    // Only .topo files should go through the Topo analysis pipeline
    return uri.size() >= 5 && uri.substr(uri.size() - 5) == ".topo";
}

static bool isTopoToml(const std::string& uri) {
    // Check if the URI points to a Topo.toml file
    return uri.size() >= 9 && uri.substr(uri.size() - 9) == "Topo.toml";
}

void LSPServer::handleDidOpen(const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto text = params["textDocument"]["text"].get<std::string>();
    documents_[uri] = text;

    // On first file open, if no host index was loaded from workspace root,
    // try to discover Topo.toml by walking up from the file's directory.
    if (hostIndex_.empty()) {
        discoverProjectConfig(uriToPath(uri));
    }

    if (isTopoToml(uri)) {
        validateTopoToml(uri, uriToPath(uri));
    } else if (isTopoFile(uri)) {
        analyzeDocument(uri);
        publishDiagnostics(uri);
    }
}

void LSPServer::handleDidChange(const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto docIt = documents_.find(uri);
    if (docIt == documents_.end()) return;

    // Apply each content change in order (LSP spec requirement)
    const auto& changes = params["contentChanges"];
    for (const auto& change : changes) {
        applyContentChange(docIt->second, change);
    }
    if (isTopoToml(uri)) {
        validateTopoToml(uri, uriToPath(uri));
    } else if (isTopoFile(uri)) {
        analyzeDocument(uri);
        publishDiagnostics(uri);
    }
}

void LSPServer::applyContentChange(std::string& doc, const json& change) {
    if (!change.contains("range")) {
        // Full replacement (fallback)
        doc = change["text"].get<std::string>();
        return;
    }

    // Incremental: convert LSP range (line/character) to byte offsets.
    // Topo source is ASCII, so character offset == byte offset within a line.
    // TODO: handle UTF-16 surrogate pairs if Topo ever supports non-ASCII
    const auto& range = change["range"];
    int startLine = range["start"]["line"].get<int>();
    int startChar = range["start"]["character"].get<int>();
    int endLine = range["end"]["line"].get<int>();
    int endChar = range["end"]["character"].get<int>();

    // Convert (line, char) to byte offset
    auto posToOffset = [&](int line, int character) -> size_t {
        size_t offset = 0;
        int curLine = 0;
        for (size_t i = 0; i < doc.size(); ++i) {
            if (curLine == line) {
                offset = i + static_cast<size_t>(character);
                break;
            }
            if (doc[i] == '\n') {
                ++curLine;
            }
        }
        if (curLine < line) {
            // Past end of document
            offset = doc.size();
        }
        return std::min(offset, doc.size());
    };

    size_t startOffset = posToOffset(startLine, startChar);
    size_t endOffset = posToOffset(endLine, endChar);

    const std::string& newText = change["text"].get_ref<const std::string&>();
    doc.replace(startOffset, endOffset - startOffset, newText);
}

void LSPServer::handleDidClose(const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    documents_.erase(uri);
    analysisCache_.erase(uri);

    // Clear diagnostics for closed document
    pendingNotifications_.push_back(json{{"jsonrpc", "2.0"},
                                         {"method", "textDocument/publishDiagnostics"},
                                         {"params", {{"uri", uri}, {"diagnostics", json::array()}}}});
}

// ============================================================================
// Topo.toml validation
// ============================================================================

void LSPServer::validateTopoToml(const std::string& uri, const std::string& filePath) {
    toml::table tbl;
    try {
        tbl = toml::parse_file(filePath);
    } catch (const toml::parse_error&) {
        // Parse error — report at line 1
        json diagArray = json::array();
        diagArray.push_back(
            {{"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 1}}}}},
             {"severity", 1},
             {"source", "topo"},
             {"message", "Failed to parse Topo.toml"}});
        pendingNotifications_.push_back(json{{"jsonrpc", "2.0"},
                                             {"method", "textDocument/publishDiagnostics"},
                                             {"params", {{"uri", uri}, {"diagnostics", diagArray}}}});
        return;
    }

    bool adaptiveEnabled = tbl["adaptive"]["enabled"].value_or(false);
    bool embedIR = tbl["build"]["embed_ir"].value_or(false);
    bool parallelInstrument = tbl["parallel"]["instrument"].value_or(false);
    std::string language = tbl["build"]["language"].value_or(std::string(""));
    std::string outputType = tbl["build"]["output_type"].value_or(std::string(""));
    std::string builderMode = tbl["builder"]["mode"].value_or(std::string(""));

    auto validation =
        topo::validateConfig(adaptiveEnabled, embedIR, parallelInstrument, language, outputType, builderMode);

    json diagArray = json::array();
    for (const auto& err : validation.errors) {
        int severity = (err.level == topo::ConfigErrorLevel::Error) ? 1 : 2;
        diagArray.push_back(
            {{"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 1}}}}},
             {"severity", severity},
             {"source", "topo"},
             {"message", err.message}});
    }

    pendingNotifications_.push_back(json{{"jsonrpc", "2.0"},
                                         {"method", "textDocument/publishDiagnostics"},
                                         {"params", {{"uri", uri}, {"diagnostics", diagArray}}}});
}

// ============================================================================
// Analysis pipeline
// ============================================================================

void LSPServer::analyzeDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    const std::string& source = it->second;
    std::string filepath = uriToPath(uri);

    DiagnosticEngine diag;
    Lexer lexer(source, filepath, diag);
    Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile();

    AnalysisResult result;
    result.diagnostics = diag.diagnostics();

    // If strict parse failed, try lenient mode to still provide semantic features
    if (diag.hasErrors()) {
        DiagnosticEngine lenientDiag;
        Lexer lenientLexer(source, filepath, lenientDiag);
        lenientLexer.setLenientMode(true);
        lenientLexer.setPreserveComments(true);

        Parser lenientParser(lenientLexer, lenientDiag);
        auto lenientAst = lenientParser.parseTopoFile();

        if (lenientAst && !lenientDiag.hasErrors()) {
            // Lenient parse succeeded — use it instead
            ast = std::move(lenientAst);

            // Generate Warning diagnostics for each transform record
            const auto& records = lenientLexer.transformRecords();
            result.diagnostics.clear();
            for (const auto& rec : records) {
                Diagnostic d;
                d.level = DiagLevel::Warning;
                d.location = rec.location;
                d.message = "non-canonical syntax: '" + rec.originalText + "' should be '" + rec.canonicalText + "'";
                d.code = "non-canonical-syntax";
                result.diagnostics.push_back(std::move(d));
            }

            // Format the AST to produce canonical source
            if (!records.empty()) {
                topo::format::Formatter formatter;
                result.canonicalSource =
                    formatter.format(static_cast<const TopoFile&>(*ast), source, lenientLexer.comments());
            }

            // Continue to semantic analysis below with the lenient AST
            diag = std::move(lenientDiag);
        }
    }

    if (ast) {
        const auto& root = static_cast<const TopoFile&>(*ast);

        // Resolve full import chain using ImportResolver (recursive).
        // Normalize filepath to forward slashes to match ImportResolver.
        std::string normalizedPath = filepath;
        for (auto& ch : normalizedPath) {
            if (ch == '\\') ch = '/';
        }

        DiagnosticEngine importDiag;
        ImportResolver resolver(importDiag);
        auto modules = resolver.resolve({normalizedPath});

        // `modules` is post-order with deps first and the root (current file)
        // last. Drive the dep loop and the current-file import merge from the
        // resolver's own ResolvedModule entries so the depCache keys and the
        // lookup keys are string-identical by construction. Manually rebuilding
        // the lookup key from `filepath` diverges from `directive.resolvedPath`
        // when libc++ and libstdc++ normalize `//`-prefixed paths differently
        // (the LSP test driver's "file:///" + absPath produces such inputs).
        std::unordered_map<std::string, SymbolTable> depCache;
        const ResolvedModule* currentModule = modules.empty() ? nullptr : &modules.back();

        for (const auto& mod : modules) {
            if (&mod == currentModule) continue; // analyzed below with the in-memory AST

            SymbolTable importedSymbols;
            for (const auto& directive : mod.imports) {
                auto cit = depCache.find(directive.resolvedPath);
                if (cit != depCache.end()) {
                    importedSymbols.mergeSelected(cit->second, directive.selectedSymbols);
                }
            }

            DiagnosticEngine depDiag;
            SemanticAnalyzer depSema(depDiag);
            auto depSymbols = depSema.analyze(static_cast<const TopoFile&>(*mod.ast), importedSymbols);

            depCache[mod.path] = std::move(depSymbols);
        }

        if (currentModule) {
            for (const auto& directive : currentModule->imports) {
                auto cit = depCache.find(directive.resolvedPath);
                if (cit != depCache.end()) {
                    result.symbols.mergeSelected(cit->second, directive.selectedSymbols);
                }
            }
        }

        // Analyze the current file with in-memory AST + imported symbols.
        // Preserve transform warning diagnostics from lenient fallback.
        auto savedDiagnostics = std::move(result.diagnostics);

        SemanticAnalyzer sema(diag);
        SymbolTable currentSymbols = sema.analyze(root, result.symbols);

        // Merge current file symbols into result
        result.symbols.mergeFrom(currentSymbols, /*filterInternal=*/false);
        for (const auto& cs : currentSymbols.callSites()) {
            result.symbols.addCallSite(cs);
        }

        // Combine: transform warnings first, then any semantic diagnostics
        result.diagnostics = std::move(savedDiagnostics);
        for (const auto& d : diag.diagnostics()) {
            result.diagnostics.push_back(d);
        }

        result.ast = std::unique_ptr<TopoFile>(static_cast<TopoFile*>(ast.release()));
    }

    analysisCache_[uri] = std::move(result);
}

void LSPServer::publishDiagnostics(const std::string& uri) {
    auto it = analysisCache_.find(uri);
    json diagArray = json::array();

    if (it != analysisCache_.end()) {
        auto docIt = documents_.find(uri);

        for (const auto& d : it->second.diagnostics) {
            int severity = (d.level == DiagLevel::Error) ? 1 : 2;
            int line = std::max(0, d.location.line - 1);
            int col = std::max(0, d.location.column - 1);

            // Use real end position when available, otherwise fall back
            // to identifier length estimation
            int endLine = line;
            int endCol = col;
            if (d.location.endLine > 0) {
                endLine = std::max(0, d.location.endLine - 1);
                endCol = std::max(0, d.location.endColumn - 1);
            } else if (docIt != documents_.end()) {
                int len = identifierLengthAt(docIt->second, d.location.line, d.location.column);
                endCol = col + std::max(1, len);
            } else {
                endCol = col + 1;
            }

            json diag = {{"range",
                          {{"start", {{"line", line}, {"character", col}}},
                           {"end", {{"line", endLine}, {"character", endCol}}}}},
                         {"severity", severity},
                         {"source", "topo"},
                         {"message", d.message}};

            if (!d.code.empty()) {
                diag["code"] = d.code;
            }

            diagArray.push_back(std::move(diag));
        }
    }

    pendingNotifications_.push_back(json{{"jsonrpc", "2.0"},
                                         {"method", "textDocument/publishDiagnostics"},
                                         {"params", {{"uri", uri}, {"diagnostics", diagArray}}}});
}

// ============================================================================
// Formatting
// ============================================================================

json LSPServer::handleFormatting(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto docIt = documents_.find(uri);
    if (docIt == documents_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    const std::string& source = docIt->second;
    std::string filepath = uriToPath(uri);

    // Re-lex with comment preservation
    DiagnosticEngine diag;
    Lexer lexer(source, filepath, diag);
    lexer.setPreserveComments(true);
    Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile();

    // If parse fails, return empty edits (avoid breaking the file)
    if (!ast || diag.hasErrors()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    Formatter formatter;
    std::string formatted = formatter.format(static_cast<const TopoFile&>(*ast), source, lexer.comments());

    // Count lines in original source for full-document replacement range
    int lineCount = 0;
    for (char c : source) {
        if (c == '\n') ++lineCount;
    }
    if (!source.empty() && source.back() != '\n') ++lineCount;

    json edits = json::array();
    edits.push_back(
        {{"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", lineCount}, {"character", 0}}}}},
         {"newText", formatted}});

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", edits}};
}

json LSPServer::handleRangeFormatting(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto docIt = documents_.find(uri);
    if (docIt == documents_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    const std::string& source = docIt->second;
    std::string filepath = uriToPath(uri);

    int startLine = params["range"]["start"]["line"].get<int>() + 1;
    int endLine = params["range"]["end"]["line"].get<int>() + 1;

    DiagnosticEngine diag;
    Lexer lexer(source, filepath, diag);
    lexer.setPreserveComments(true);
    Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile();

    if (!ast || diag.hasErrors()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    Formatter formatter;
    std::string formatted =
        formatter.formatRange(static_cast<const TopoFile&>(*ast), source, lexer.comments(), startLine, endLine);

    // Full-document replacement (formatRange still formats everything for consistency)
    int lineCount = 0;
    for (char c : source) {
        if (c == '\n') ++lineCount;
    }
    if (!source.empty() && source.back() != '\n') ++lineCount;

    json edits = json::array();
    edits.push_back(
        {{"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", lineCount}, {"character", 0}}}}},
         {"newText", formatted}});

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", edits}};
}

// ============================================================================
// Hover
// ============================================================================

json LSPServer::handleHover(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    int line = params["position"]["line"].get<int>() + 1;
    int col = params["position"]["character"].get<int>() + 1;

    auto docIt = documents_.find(uri);
    auto cacheIt = analysisCache_.find(uri);

    if (docIt == documents_.end() || cacheIt == analysisCache_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    std::string ident = identifierAtPosition(docIt->second, line, col);
    if (ident.empty()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    const auto& symbols = cacheIt->second.symbols;
    auto candidates = qualifiedCandidates(uri, ident);

    for (const auto& qname : candidates) {
        if (auto* fn = symbols.findFunction(qname)) {
            // Build hover content
            std::string visStr = visibilityName(fn->visibility);
            // Reconstruct signature from FunctionSymbol
            std::string sig;
            if (fn->isMultiReturn) {
                sig += fn->simpleName + "(";
                for (size_t i = 0; i < fn->params.size(); ++i) {
                    if (i > 0) sig += ", ";
                    sig += fn->params[i].toString();
                }
                sig += ") -> (";
                for (size_t i = 0; i < fn->returnParams.size(); ++i) {
                    if (i > 0) sig += ", ";
                    sig += fn->returnParams[i].toString();
                }
                sig += ")";
            } else {
                sig += fn->returnType.toString() + " " + fn->simpleName + "(";
                for (size_t i = 0; i < fn->params.size(); ++i) {
                    if (i > 0) sig += ", ";
                    sig += fn->params[i].toString();
                }
                sig += ")";
                if (fn->isConst) sig += " const";
            }

            std::string content =
                "```topo\n" + visStr + ": " + sig + "\n```\n\n**Qualified**: `" + fn->qualifiedName + "`";

            if (fn->priority != PriorityLevel::Normal) {
                content += "\n\n**Priority**: " + std::string(priorityLevelName(fn->priority));
            }

            if (fn->cardinality) {
                content += "\n\n**Cardinality**: ";
                if (fn->cardinality->min >= 0) content += std::to_string(fn->cardinality->min);
                content += "..";
                if (fn->cardinality->max >= 0) content += std::to_string(fn->cardinality->max);
            }

            if (fn->accessPattern != AccessPattern::None) {
                const char* patName = "none";
                switch (fn->accessPattern) {
                case AccessPattern::Streaming: patName = "streaming"; break;
                case AccessPattern::Random: patName = "random"; break;
                case AccessPattern::Tiled: patName = "tiled"; break;
                case AccessPattern::GatherScatter: patName = "gather_scatter"; break;
                default: break;
                }
                content += "\n\n**Access**: " + std::string(patName);
                if (fn->accessPattern == AccessPattern::Tiled && fn->tiledSize > 0)
                    content += " (" + std::to_string(fn->tiledSize) + ")";
            }

            if (fn->bindingTarget) {
                content += "\n\n→ binds to `" + *fn->bindingTarget + "`";
            }

            // Add host language definition location if available
            if (!fn->hasLogicBlock) {
                if (bridge_ && bridge_->isAvailable()) {
                    auto hoverInfo = bridge_->getHoverInfo(fn->qualifiedName, projectConfig_.sources);
                    if (hoverInfo) {
                        content += "\n\n**" + bridge_->displayName() + " signature**:\n```" +
                                   bridge_->languageId() + "\n" + *hoverInfo + "\n```";
                    }
                }
                if (auto* hostLoc = hostIndex_.find(fn->qualifiedName)) {
                    content += "\n\n**" + (bridge_ ? bridge_->displayName() : "Host") +
                               " definition**: `" + hostLoc->file + ":" + std::to_string(hostLoc->line) + "`";
                }
            }

            return json{
                {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", content}}}}}};
        }

        // Check logic blocks
        if (auto* lb = symbols.findLogicBlock(qname)) {
            std::string content = "```topo\nfn " + lb->simpleName + " { ... }\n```";
            if (lb->isPipeline) {
                content += "\n\n**Pipeline** logic block";
            }
            content += "\n\n**Operations**: " + std::to_string(lb->calledFunctions.size());

            return json{
                {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", content}}}}}};
        }

        // Check type aliases
        if (auto* ta = symbols.findTypeAlias(qname)) {
            std::string content = "```topo\nusing " + ta->name + " = " + ta->targetType.toString() + "\n```";
            return json{
                {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", content}}}}}};
        }
    }

    // Check class symbols
    for (const auto& qname : candidates) {
        if (auto* cls = symbols.findClassSymbol(qname)) {
            std::string sig = "class " + cls->simpleName;
            if (cls->baseClass) {
                sig += " : public " + cls->baseClass->toString();
            }
            std::string visStr = visibilityName(cls->visibility);
            std::string content =
                "```topo\n" + visStr + ": " + sig + "\n```\n\n**Qualified**: `" + cls->qualifiedName + "`";
            content += "\n\n**Members**: " + std::to_string(cls->memberFunctions.size()) + " functions, " +
                       std::to_string(cls->memberVars.size()) + " fields";
            return json{
                {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", content}}}}}};
        }
    }

    // Check std::import types (by simple name)
    for (const auto& imp : symbols.imports()) {
        if (imp.typeName == ident) {
            std::string content = "```topo\nstd::import(\"" + imp.path + "\", " + imp.typeName + ")\n```";
            content += "\n\n**Imported from**: `" + imp.path + "`";

            // Query host language bridge for type details
            if (bridge_ && bridge_->isAvailable()) {
                auto hoverInfo = bridge_->getHoverInfo(imp.typeName, projectConfig_.sources);
                if (hoverInfo) {
                    content += "\n\n**" + bridge_->displayName() + " type**:\n```" +
                               bridge_->languageId() + "\n" + *hoverInfo + "\n```";
                }
            }

            return json{
                {"jsonrpc", "2.0"}, {"id", id}, {"result", {{"contents", {{"kind", "markdown"}, {"value", content}}}}}};
        }
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
}

// ============================================================================
// Go to Definition
// ============================================================================

json LSPServer::handleDefinition(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    int line = params["position"]["line"].get<int>() + 1;
    int col = params["position"]["character"].get<int>() + 1;

    // Topo.toml: jump to files referenced by string paths
    auto filepath = uriToPath(uri);
    if (filepath.size() >= 9 && filepath.substr(filepath.size() - 9) == "Topo.toml") {
        return handleTomlDefinition(id, uri, line, col);
    }

    auto docIt = documents_.find(uri);
    auto cacheIt = analysisCache_.find(uri);

    if (docIt == documents_.end() || cacheIt == analysisCache_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    // Feature 1: Import Go-to-Definition — jump to imported .topo file
    if (cacheIt->second.ast) {
        for (const auto& decl : cacheIt->second.ast->declarations) {
            if (decl->kind == ASTKind::FileImport) {
                const auto& imp = static_cast<const FileImport&>(*decl);
                if (imp.location.line == line) {
                    fs::path currentDir = fs::path(uriToPath(uri)).parent_path();
                    fs::path importPath = currentDir / (imp.path + ".topo");
                    if (fs::exists(importPath)) {
                        std::string tgtUri = pathToUri(importPath.string());
                        return json{{"jsonrpc", "2.0"},
                                    {"id", id},
                                    {"result",
                                     {{"uri", tgtUri},
                                      {"range",
                                       {{"start", {{"line", 0}, {"character", 0}}},
                                        {"end", {{"line", 0}, {"character", 0}}}}}}}};
                    }
                }
            }
        }
    }

    std::string ident = identifierAtPosition(docIt->second, line, col);
    if (ident.empty()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    const auto& symbols = cacheIt->second.symbols;
    auto candidates = qualifiedCandidates(uri, ident);

    for (const auto& qname : candidates) {
        const SourceLocation* loc = nullptr;
        bool isFunctionDecl = false;
        std::string qualifiedName;

        if (auto* fn = symbols.findFunction(qname)) {
            loc = &fn->location;
            isFunctionDecl = !fn->hasLogicBlock;
            qualifiedName = fn->qualifiedName;
        } else if (auto* lb = symbols.findLogicBlock(qname)) {
            loc = &lb->location;
        } else if (auto* ta = symbols.findTypeAlias(qname)) {
            loc = &ta->location;
        } else if (auto* cls = symbols.findClassSymbol(qname)) {
            loc = &cls->location;
        } else if (auto* cs = symbols.findConstraintSymbol(qname)) {
            loc = &cs->location;
        }

        if (loc) {
            // For function declarations (no fn block), try host language resolution
            // Feature 4: skip host index when cursor is inside a fn block
            if (isFunctionDecl && !isInsideFnBlock(docIt->second, line, col)) {
                if (bridge_ && bridge_->isAvailable()) {
                    auto result = bridge_->findDefinition(qualifiedName, projectConfig_.sources);
                    if (result) {
                        std::string tgtUri = pathToUri(result->file);
                        return json{{"jsonrpc", "2.0"},
                                    {"id", id},
                                    {"result",
                                     {{"uri", tgtUri},
                                      {"range",
                                       {{"start", {{"line", result->line}, {"character", result->column}}},
                                        {"end", {{"line", result->line}, {"character", result->column}}}}}}}};
                    }
                }

                // Fallback: regex-based host index
                if (auto* hostLoc = hostIndex_.find(qualifiedName)) {
                    std::string tgtUri = pathToUri(hostLoc->file);
                    int tgtLine = std::max(0, hostLoc->line - 1);
                    return json{{"jsonrpc", "2.0"},
                                {"id", id},
                                {"result",
                                 {{"uri", tgtUri},
                                  {"range",
                                   {{"start", {{"line", tgtLine}, {"character", 0}}},
                                    {"end", {{"line", tgtLine}, {"character", 0}}}}}}}};
                }
            }

            // Fallback: .topo declaration location
            int tgtLine = std::max(0, loc->line - 1);
            int tgtCol = std::max(0, loc->column - 1);
            std::string tgtUri = loc->file.empty() ? uri : pathToUri(loc->file);

            return json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result",
                         {{"uri", tgtUri},
                          {"range",
                           {{"start", {{"line", tgtLine}, {"character", tgtCol}}},
                            {"end", {{"line", tgtLine}, {"character", tgtCol}}}}}}}};
        }
    }

    // Check std::import types — jump directly to host language definition.
    // The import path (e.g. "types.h") is the primary resolution hint:
    // resolve it to an actual file, then find the type name inside it.
    for (const auto& imp : symbols.imports()) {
        if (imp.typeName == ident) {
            // 1. Resolve importPath to an actual file on disk.
            //    Search relative to the .topo file, its parent (topo/ and
            //    include/ are often siblings), configured includeDirs, and
            //    the workspace root.
            std::string resolvedFile;
            {
                fs::path topoDir = fs::path(uriToPath(uri)).parent_path();
                std::vector<fs::path> searchDirs = {
                    topoDir,
                    topoDir.parent_path(),
                    topoDir.parent_path() / "include",
                };
                for (const auto& d : projectConfig_.includeDirs)
                    searchDirs.push_back(d);
                if (!workspaceRoot_.empty()) {
                    searchDirs.push_back(workspaceRoot_);
                    searchDirs.push_back(fs::path(workspaceRoot_) / "include");
                }

                for (const auto& dir : searchDirs) {
                    fs::path candidate = dir / imp.path;
                    if (fs::exists(candidate)) {
                        resolvedFile = fs::canonical(candidate).string();
                        break;
                    }
                }
            }

            // 2. If file found, search for the type name in it.
            if (!resolvedFile.empty()) {
                std::ifstream file(resolvedFile);
                if (file.is_open()) {
                    std::string fileLine;
                    int lineNo = 0;
                    while (std::getline(file, fileLine)) {
                        ++lineNo;
                        auto pos = fileLine.find(imp.typeName);
                        if (pos == std::string::npos) continue;
                        // Word-boundary check
                        bool leftOk = (pos == 0) || (!std::isalnum(static_cast<unsigned char>(fileLine[pos - 1])) &&
                                                     fileLine[pos - 1] != '_');
                        bool rightOk =
                            (pos + imp.typeName.size() >= fileLine.size()) ||
                            (!std::isalnum(static_cast<unsigned char>(fileLine[pos + imp.typeName.size()])) &&
                             fileLine[pos + imp.typeName.size()] != '_');
                        if (leftOk && rightOk) {
                            std::string tgtUri = pathToUri(resolvedFile);
                            return json{{"jsonrpc", "2.0"},
                                        {"id", id},
                                        {"result",
                                         {{"uri", tgtUri},
                                          {"range",
                                           {{"start", {{"line", lineNo - 1}, {"character", static_cast<int>(pos)}}},
                                            {"end",
                                             {{"line", lineNo - 1},
                                              {"character", static_cast<int>(pos + imp.typeName.size())}}}}}}}};
                        }
                    }
                }
            }

            // 3. Bridge fallback (workspace/symbol + regex scan)
            std::optional<SymbolResult> hostDef;
            if (bridge_ && bridge_->isAvailable()) {
                hostDef = bridge_->findTypeDefinition(
                    imp.typeName, projectConfig_.sources, projectConfig_.includeDirs);
            }

            if (hostDef) {
                std::string tgtUri = pathToUri(hostDef->file);
                return json{{"jsonrpc", "2.0"},
                            {"id", id},
                            {"result",
                             {{"uri", tgtUri},
                              {"range",
                               {{"start", {{"line", hostDef->line}, {"character", hostDef->column}}},
                                {"end", {{"line", hostDef->line}, {"character", hostDef->column}}}}}}}};
            }

            // 4. Fallback: jump to .topo import declaration
            int tgtLine = std::max(0, imp.location.line - 1);
            int tgtCol = std::max(0, imp.location.column - 1);
            std::string tgtUri = imp.location.file.empty() ? uri : pathToUri(imp.location.file);

            return json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result",
                         {{"uri", tgtUri},
                          {"range",
                           {{"start", {{"line", tgtLine}, {"character", tgtCol}}},
                            {"end", {{"line", tgtLine}, {"character", tgtCol}}}}}}}};
        }
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
}

// ============================================================================
// Topo.toml Go to Definition
// ============================================================================

json LSPServer::handleTomlDefinition(const json& id, const std::string& uri, int line, int col) {
    auto docIt = documents_.find(uri);
    if (docIt == documents_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    // Extract the line content
    const std::string& source = docIt->second;
    std::istringstream stream(source);
    std::string lineStr;
    int currentLine = 0;
    while (std::getline(stream, lineStr)) {
        ++currentLine;
        if (currentLine == line) break;
    }
    if (currentLine != line) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    // Find the quoted string surrounding the cursor (0-based col index)
    int idx = col - 1;
    // Find the opening quote before or at cursor
    int qStart = -1, qEnd = -1;
    for (int i = idx; i >= 0; --i) {
        if (lineStr[i] == '"') {
            qStart = i;
            break;
        }
    }
    if (qStart < 0) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }
    for (int i = qStart + 1; i < static_cast<int>(lineStr.size()); ++i) {
        if (lineStr[i] == '"') {
            qEnd = i;
            break;
        }
    }
    if (qEnd < 0 || idx > qEnd) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    std::string pathStr = lineStr.substr(qStart + 1, qEnd - qStart - 1);
    if (pathStr.empty()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
    }

    // Resolve relative to the Topo.toml directory
    fs::path tomlDir = fs::path(uriToPath(uri)).parent_path();
    fs::path target = tomlDir / pathStr;

    // For directories, just open the directory (VS Code will show it)
    // For files, jump to line 0
    if (fs::exists(target)) {
        std::string tgtUri;
        if (fs::is_directory(target)) {
            // If it's a directory, try to find an index-like file
            // Otherwise just return null (VS Code can't open dirs via LSP)
            return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
        }
        tgtUri = pathToUri(fs::canonical(target).string());
        return json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result",
             {{"uri", tgtUri},
              {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 0}}}}}}}};
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", nullptr}};
}

// ============================================================================
// Find References
// ============================================================================

json LSPServer::handleReferences(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    int line = params["position"]["line"].get<int>() + 1;
    int col = params["position"]["character"].get<int>() + 1;

    auto docIt = documents_.find(uri);
    auto cacheIt = analysisCache_.find(uri);

    if (docIt == documents_.end() || cacheIt == analysisCache_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    std::string ident = identifierAtPosition(docIt->second, line, col);
    if (ident.empty()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    const auto& symbols = cacheIt->second.symbols;
    auto candidates = qualifiedCandidates(uri, ident);

    json locations = json::array();

    for (const auto& qname : candidates) {
        // Check if this qualified name exists
        bool found = symbols.findFunction(qname) || symbols.findLogicBlock(qname) || symbols.findClassSymbol(qname) ||
                     symbols.findTypeAlias(qname) || symbols.findConstraintSymbol(qname);
        if (!found) continue;

        // Collect call sites where callee matches
        for (const auto& cs : symbols.callSites()) {
            if (cs.callee == qname) {
                int refLine = std::max(0, cs.loc.line - 1);
                int refCol = std::max(0, cs.loc.column - 1);
                std::string refUri = cs.loc.file.empty() ? uri : pathToUri(cs.loc.file);
                locations.push_back({{"uri", refUri},
                                     {"range",
                                      {{"start", {{"line", refLine}, {"character", refCol}}},
                                       {"end", {{"line", refLine}, {"character", refCol}}}}}});
            }
        }
        break; // Found the symbol, stop searching candidates
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", locations}};
}

// ============================================================================
// Completion
// ============================================================================

json LSPServer::handleCompletion(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    int line = params["position"]["line"].get<int>() + 1;
    int col = params["position"]["character"].get<int>() + 1;

    auto docIt = documents_.find(uri);
    auto cacheIt = analysisCache_.find(uri);

    json items = json::array();

    if (docIt == documents_.end() || cacheIt == analysisCache_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", items}}}};
    }

    // Get the partial identifier being typed
    std::string prefix = identifierAtPosition(docIt->second, line, col);
    // Convert prefix to lowercase for case-insensitive matching
    std::string lowerPrefix = prefix;
    std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(), ::tolower);

    const auto& symbols = cacheIt->second.symbols;

    // Functions
    for (const auto& [qname, fn] : symbols.functions()) {
        std::string lowerSimple = fn.simpleName;
        std::transform(lowerSimple.begin(), lowerSimple.end(), lowerSimple.begin(), ::tolower);

        if (lowerPrefix.empty() || lowerSimple.find(lowerPrefix) == 0 || qname.find(prefix) != std::string::npos) {
            std::string detail = visibilityName(fn.visibility);
            detail += ": " + fn.returnType.toString();

            items.push_back({{"label", fn.simpleName},
                             {"kind", 3}, // Function
                             {"detail", detail},
                             {"documentation", qname}});
        }
    }

    // Type aliases
    for (const auto& [name, ta] : symbols.typeAliases()) {
        std::string lowerName = ta.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        if (lowerPrefix.empty() || lowerName.find(lowerPrefix) == 0) {
            items.push_back({{"label", ta.name},
                             {"kind", 25}, // TypeParameter
                             {"detail", "= " + ta.targetType.toString()}});
        }
    }

    // Logic blocks
    for (const auto& [qname, lb] : symbols.logicBlocks()) {
        std::string lowerSimple = lb.simpleName;
        std::transform(lowerSimple.begin(), lowerSimple.end(), lowerSimple.begin(), ::tolower);

        if (lowerPrefix.empty() || lowerSimple.find(lowerPrefix) == 0) {
            items.push_back({{"label", lb.simpleName},
                             {"kind", 12}, // Value (logic block)
                             {"detail", lb.isPipeline ? "pipeline fn" : "fn"},
                             {"documentation", qname}});
        }
    }

    // Imported types
    for (const auto& imp : symbols.imports()) {
        if (!imp.typeName.empty()) {
            std::string lowerType = imp.typeName;
            std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);

            if (lowerPrefix.empty() || lowerType.find(lowerPrefix) == 0) {
                items.push_back({{"label", imp.typeName},
                                 {"kind", 22}, // Struct
                                 {"detail", "std::import(\"" + imp.path + "\")"}});
            }
        }
    }

    // Keywords (when no prefix or matching)
    static const std::vector<std::string> keywords = {
        "namespace",  "public",   "protected", "private",     "internal", "fn",       "stage",    "using",
        "import",     "const",    "void",      "class",       "static",   "explicit", "template", "typename",
        "constraint", "requires", "adapt",     "instantiate", "for",      "comptime", "typefn",   "match",
        "if",         "else",     "return",    "operator",    "priority", "external", "handler",  "flow"};

    for (const auto& kw : keywords) {
        if (lowerPrefix.empty() || kw.find(lowerPrefix) == 0) {
            items.push_back({
                {"label", kw}, {"kind", 14} // Keyword
            });
        }
    }

    // stdlib bridging types as completion candidates.
    // Surfaced as kind=7 (Class / type-like) with a "stdlib type" detail so
    // they sort separately from regular keywords in most editors.
    // Single-sourced from the stdlib type table so any newly added type
    // (width-extension scalars, record, future composites) autocompletes
    // without a parallel hand-maintained list drifting out of sync.
    for (const auto& e : topo::stdlib::allEntries()) {
        std::string name = e.keyword;
        if (lowerPrefix.empty() || name.find(lowerPrefix) == 0) {
            items.push_back({{"label", name},
                             {"kind", 7}, // Class
                             {"detail", std::string("stdlib type — ") + e.description}});
        }
    }

    // Priority level names (for use inside priority() sections)
    static const std::vector<std::string> priorityLevels = {"critical", "high", "normal", "low", "background"};
    for (const auto& pl : priorityLevels) {
        if (lowerPrefix.empty() || pl.find(lowerPrefix) == 0) {
            items.push_back({{"label", pl},
                             {"kind", 20}, // EnumMember
                             {"detail", "priority level"}});
        }
    }

    // Data-aware optimization hint names
    static const std::vector<std::pair<std::string, std::string>> hintNames = {
        {"cardinality", "data scale hint (e.g. cardinality(1k..100k))"},
        {"access", "access pattern hint (streaming/random/tiled/gather_scatter)"}};
    for (const auto& [name, detail] : hintNames) {
        if (lowerPrefix.empty() || name.find(lowerPrefix) == 0) {
            items.push_back({{"label", name},
                             {"kind", 20}, // EnumMember
                             {"detail", detail}});
        }
    }

    // Access pattern names
    static const std::vector<std::string> accessPatterns = {"streaming", "random", "tiled", "gather_scatter"};
    for (const auto& ap : accessPatterns) {
        if (lowerPrefix.empty() || ap.find(lowerPrefix) == 0) {
            items.push_back({{"label", ap},
                             {"kind", 20}, // EnumMember
                             {"detail", "access pattern"}});
        }
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"isIncomplete", false}, {"items", items}}}};
}

// ============================================================================
// Document Symbols
// ============================================================================

json LSPServer::handleDocumentSymbol(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto cacheIt = analysisCache_.find(uri);

    if (cacheIt == analysisCache_.end() || !cacheIt->second.ast) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::array()}};
    }

    json symbols = collectSymbols(*cacheIt->second.ast);
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", symbols}};
}

json LSPServer::collectSymbols(const TopoFile& root) const {
    json result = json::array();

    for (const auto& decl : root.declarations) {
        if (decl->kind == ASTKind::NamespaceDecl) {
            const auto& ns = static_cast<const NamespaceDecl&>(*decl);
            int line = std::max(0, ns.location.line - 1);
            int col = std::max(0, ns.location.column - 1);

            json children = collectNamespaceSymbols(ns, "");

            result.push_back(
                {{"name", ns.pathString()},
                 {"kind", 3}, // Namespace
                 {"range",
                  {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col}}}}},
                 {"selectionRange",
                  {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col}}}}},
                 {"children", children}});
        } else if (decl->kind == ASTKind::TypeAliasDecl) {
            const auto& ta = static_cast<const TypeAliasDecl&>(*decl);
            int line = std::max(0, ta.location.line - 1);
            int col = std::max(0, ta.location.column - 1);

            result.push_back(
                {{"name", ta.name},
                 {"kind", 26}, // TypeParameter
                 {"range",
                  {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col}}}}},
                 {"selectionRange",
                  {{"start", {{"line", line}, {"character", col}}}, {"end", {{"line", line}, {"character", col}}}}}});
        }
    }

    return result;
}

json LSPServer::collectNamespaceSymbols(const NamespaceDecl& ns, const std::string& /*parentPath*/) const {
    json children = json::array();

    for (const auto& section : ns.sections) {
        if (section->kind == ASTKind::VisibilitySection) {
            const auto& vis = static_cast<const VisibilitySection&>(*section);
            for (const auto& member : vis.members) {
                int mLine = std::max(0, member->location.line - 1);
                int mCol = std::max(0, member->location.column - 1);

                if (member->kind == ASTKind::FunctionDecl) {
                    const auto& fn = static_cast<const FunctionDecl&>(*member);
                    children.push_back({{"name", fn.name},
                                        {"kind", 12}, // Function
                                        {"detail", fn.signature()},
                                        {"range",
                                         {{"start", {{"line", mLine}, {"character", mCol}}},
                                          {"end", {{"line", mLine}, {"character", mCol}}}}},
                                        {"selectionRange",
                                         {{"start", {{"line", mLine}, {"character", mCol}}},
                                          {"end", {{"line", mLine}, {"character", mCol}}}}}});
                } else if (member->kind == ASTKind::FunctionLogicBlock) {
                    const auto& lb = static_cast<const FunctionLogicBlock&>(*member);
                    children.push_back({{"name", "fn " + lb.name},
                                        {"kind", 12}, // Function
                                        {"detail", lb.isPipeline() ? "pipeline" : "logic block"},
                                        {"range",
                                         {{"start", {{"line", mLine}, {"character", mCol}}},
                                          {"end", {{"line", mLine}, {"character", mCol}}}}},
                                        {"selectionRange",
                                         {{"start", {{"line", mLine}, {"character", mCol}}},
                                          {"end", {{"line", mLine}, {"character", mCol}}}}}});
                } else if (member->kind == ASTKind::ClassDecl) {
                    const auto& cls = static_cast<const ClassDecl&>(*member);
                    json classChildren = json::array();

                    // Collect class member symbols
                    for (const auto& sec : cls.sections) {
                        if (sec->kind != ASTKind::VisibilitySection) continue;
                        const auto& visSec = static_cast<const VisibilitySection&>(*sec);
                        for (const auto& cm : visSec.members) {
                            int cmLine = std::max(0, cm->location.line - 1);
                            int cmCol = std::max(0, cm->location.column - 1);
                            if (cm->kind == ASTKind::FnDecl) {
                                const auto& fn = static_cast<const FnDecl&>(*cm);
                                std::string symbolName;
                                int symbolKind = 6; // Method
                                if (fn.isConstructor) {
                                    symbolName = fn.className;
                                    symbolKind = 9; // Constructor
                                } else if (fn.isDestructor) {
                                    symbolName = "~" + fn.className;
                                } else if (fn.isOperator()) {
                                    symbolName = std::string("operator") + overloadableOpName(*fn.operatorOp);
                                    symbolKind = 24; // Operator
                                } else {
                                    symbolName = fn.name;
                                }
                                classChildren.push_back({{"name", symbolName},
                                                         {"kind", symbolKind},
                                                         {"detail", fn.signature()},
                                                         {"range",
                                                          {{"start", {{"line", cmLine}, {"character", cmCol}}},
                                                           {"end", {{"line", cmLine}, {"character", cmCol}}}}},
                                                         {"selectionRange",
                                                          {{"start", {{"line", cmLine}, {"character", cmCol}}},
                                                           {"end", {{"line", cmLine}, {"character", cmCol}}}}}});
                            } else if (cm->kind == ASTKind::DataDecl) {
                                const auto& var = static_cast<const DataDecl&>(*cm);
                                classChildren.push_back({{"name", var.name},
                                                         {"kind", 8}, // Field
                                                         {"detail", var.type.toString()},
                                                         {"range",
                                                          {{"start", {{"line", cmLine}, {"character", cmCol}}},
                                                           {"end", {{"line", cmLine}, {"character", cmCol}}}}},
                                                         {"selectionRange",
                                                          {{"start", {{"line", cmLine}, {"character", cmCol}}},
                                                           {"end", {{"line", cmLine}, {"character", cmCol}}}}}});
                            }
                        }
                    }

                    children.push_back({{"name", cls.name},
                                        {"kind", 5}, // Class
                                        {"range",
                                         {{"start", {{"line", mLine}, {"character", mCol}}},
                                          {"end", {{"line", mLine}, {"character", mCol}}}}},
                                        {"selectionRange",
                                         {{"start", {{"line", mLine}, {"character", mCol}}},
                                          {"end", {{"line", mLine}, {"character", mCol}}}}},
                                        {"children", classChildren}});
                } else if (member->kind == ASTKind::NamespaceDecl) {
                    const auto& nested = static_cast<const NamespaceDecl&>(*member);
                    int nLine = std::max(0, nested.location.line - 1);
                    int nCol = std::max(0, nested.location.column - 1);
                    json nestedChildren = collectNamespaceSymbols(nested, "");
                    children.push_back({{"name", nested.pathString()},
                                        {"kind", 3}, // Namespace
                                        {"range",
                                         {{"start", {{"line", nLine}, {"character", nCol}}},
                                          {"end", {{"line", nLine}, {"character", nCol}}}}},
                                        {"selectionRange",
                                         {{"start", {{"line", nLine}, {"character", nCol}}},
                                          {"end", {{"line", nLine}, {"character", nCol}}}}},
                                        {"children", nestedChildren}});
                }
            }
        }
    }

    return children;
}

// ============================================================================
// Symbol resolution helpers
// ============================================================================

std::string LSPServer::identifierAtPosition(const std::string& source, int line, int column) const {
    // Split source into lines
    std::istringstream stream(source);
    std::string lineStr;
    int currentLine = 0;
    while (std::getline(stream, lineStr)) {
        ++currentLine;
        if (currentLine == line) break;
    }

    if (currentLine != line) return "";

    // column is 1-based; convert to 0-based index
    int idx = column - 1;
    if (idx < 0 || idx >= static_cast<int>(lineStr.size())) return "";

    // Expand left and right to find the full identifier
    auto isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    auto isQualSep = [](const std::string& s, int i) {
        return i + 1 < static_cast<int>(s.size()) && s[i] == ':' && s[i + 1] == ':';
    };

    // If cursor is on ':', try to move to adjacent identifier
    if (!isIdentChar(lineStr[idx]) && lineStr[idx] != ':') return "";

    int start = idx;
    int end = idx;

    // Expand right
    while (end < static_cast<int>(lineStr.size())) {
        if (isIdentChar(lineStr[end])) {
            ++end;
        } else if (isQualSep(lineStr, end)) {
            end += 2; // skip ::
        } else {
            break;
        }
    }

    // Expand left
    while (start > 0) {
        if (isIdentChar(lineStr[start - 1])) {
            --start;
        } else if (start >= 2 && lineStr[start - 1] == ':' && lineStr[start - 2] == ':') {
            start -= 2;
        } else {
            break;
        }
    }

    // Also handle dot-separated names (e.g. telemetry.init)
    while (end < static_cast<int>(lineStr.size()) && lineStr[end] == '.' &&
           end + 1 < static_cast<int>(lineStr.size()) && isIdentChar(lineStr[end + 1])) {
        ++end; // skip dot
        while (end < static_cast<int>(lineStr.size()) && isIdentChar(lineStr[end]))
            ++end;
    }

    while (start > 0 && lineStr[start - 1] == '.' && start >= 2 && isIdentChar(lineStr[start - 2])) {
        start -= 1; // skip dot
        while (start > 0 && isIdentChar(lineStr[start - 1]))
            --start;
    }

    if (start >= end) return "";
    return lineStr.substr(start, end - start);
}

int LSPServer::identifierLengthAt(const std::string& source, int line, int column) const {
    std::istringstream stream(source);
    std::string lineStr;
    int currentLine = 0;
    while (std::getline(stream, lineStr)) {
        ++currentLine;
        if (currentLine == line) break;
    }
    if (currentLine != line) return 0;

    int idx = column - 1;
    if (idx < 0 || idx >= static_cast<int>(lineStr.size())) return 0;

    auto isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    if (!isIdentChar(lineStr[idx])) return 0;

    int len = 0;
    while (idx + len < static_cast<int>(lineStr.size()) && isIdentChar(lineStr[idx + len])) {
        ++len;
    }
    return len;
}

std::vector<std::string> LSPServer::qualifiedCandidates(const std::string& uri, const std::string& identifier) const {
    std::vector<std::string> result;

    // Normalize dots to :: for symbol table lookup
    std::string normalized = identifier;
    for (auto& c : normalized) {
        if (c == '.') c = ':';
    }
    // Convert single colons from dot replacement to ::
    std::string fixed;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == ':' && (i == 0 || normalized[i - 1] != ':') &&
            (i + 1 >= normalized.size() || normalized[i + 1] != ':')) {
            fixed += "::";
        } else {
            fixed += normalized[i];
        }
    }

    // If already qualified (contains ::), try as-is first
    if (fixed.find("::") != std::string::npos) {
        result.push_back(fixed);
    }

    // Try with namespace prefixes from the document
    auto cacheIt = analysisCache_.find(uri);
    if (cacheIt != analysisCache_.end() && cacheIt->second.ast) {
        // Collect all namespace paths from the AST
        for (const auto& decl : cacheIt->second.ast->declarations) {
            if (decl->kind == ASTKind::NamespaceDecl) {
                const auto& ns = static_cast<const NamespaceDecl&>(*decl);
                std::string nsPath = ns.pathString();
                result.push_back(nsPath + "::" + fixed);
            }
        }
    }

    // Try the bare identifier
    if (fixed.find("::") == std::string::npos) {
        result.push_back(fixed);
    }

    // Also try matching against all known symbols by simple name
    if (cacheIt != analysisCache_.end()) {
        const auto& symbols = cacheIt->second.symbols;
        std::string simpleName = identifier;
        // Get the last component
        auto lastDot = simpleName.rfind('.');
        auto lastColon = simpleName.rfind("::");
        if (lastDot != std::string::npos)
            simpleName = simpleName.substr(lastDot + 1);
        else if (lastColon != std::string::npos)
            simpleName = simpleName.substr(lastColon + 2);

        for (const auto& [qname, fn] : symbols.functions()) {
            if (fn.simpleName == simpleName) {
                result.push_back(qname);
            }
        }
        for (const auto& [qname, lb] : symbols.logicBlocks()) {
            if (lb.simpleName == simpleName) {
                result.push_back(qname);
            }
        }
        for (const auto& [qname, cls] : symbols.classSymbols()) {
            if (cls.simpleName == simpleName) {
                result.push_back(qname);
            }
        }
        for (const auto& [name, ta] : symbols.typeAliases()) {
            if (ta.name == simpleName) {
                result.push_back(name);
            }
        }
        for (const auto& [name, cs] : symbols.constraintSymbols()) {
            if (cs.simpleName == simpleName) {
                result.push_back(name);
            }
        }
    }

    return result;
}

std::string LSPServer::findNamespaceAtLine(const TopoFile& root, int line) const {
    for (const auto& decl : root.declarations) {
        if (decl->kind == ASTKind::NamespaceDecl) {
            const auto& ns = static_cast<const NamespaceDecl&>(*decl);
            if (ns.location.line <= line) {
                return ns.pathString();
            }
        }
    }
    return "";
}

// ============================================================================
// fn-block detection helper
// ============================================================================

bool LSPServer::isInsideFnBlock(const std::string& source, int line, int /*col*/) const {
    std::istringstream stream(source);
    std::string lineStr;
    int currentLine = 0;
    int fnBraceDepth = 0;
    bool insideFn = false;

    std::regex fnRegex(R"(^\s*fn\s+[a-zA-Z_]\w*\s*\{)");

    while (std::getline(stream, lineStr)) {
        ++currentLine;
        if (currentLine > line) break;

        // Skip line comments
        auto slashPos = lineStr.find("//");
        std::string effective = (slashPos != std::string::npos) ? lineStr.substr(0, slashPos) : lineStr;

        // Check for fn block opening
        if (!insideFn) {
            if (std::regex_search(effective, fnRegex)) {
                insideFn = true;
                fnBraceDepth = 0;
                // Count braces on this line
                for (char c : effective) {
                    if (c == '{')
                        ++fnBraceDepth;
                    else if (c == '}')
                        --fnBraceDepth;
                }
                if (fnBraceDepth <= 0) insideFn = false;
                continue;
            }
        } else {
            // Track braces inside fn block
            for (char c : effective) {
                if (c == '{')
                    ++fnBraceDepth;
                else if (c == '}') {
                    --fnBraceDepth;
                    if (fnBraceDepth <= 0) {
                        insideFn = false;
                        break;
                    }
                }
            }
        }
    }

    return insideFn;
}

// ============================================================================
// Code Actions (Quick Fix)
// ============================================================================

json LSPServer::handleCodeAction(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto docIt = documents_.find(uri);
    json actions = json::array();

    if (docIt == documents_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", actions}};
    }

    const auto& diagnostics = params["context"]["diagnostics"];
    for (const auto& diag : diagnostics) {
        if (!diag.contains("code")) continue;
        std::string code = diag["code"].get<std::string>();
        std::string message = diag.value("message", "");

        if (code == "unknown-type") {
            // Suggest adding a using declaration for common types
            std::string typeName = extractQuotedName(message);
            if (typeName.empty()) continue;

            // Only suggest for types that look like C++ builtins
            std::string target;
            if (typeName == "int") {
                target = "std::cpp17::int";
            } else if (typeName == "bool") {
                target = "std::cpp17::bool";
            } else {
                continue;
            }

            int insertLine = findInsertionLine(docIt->second);
            std::string insertText = "using " + typeName + " = " + target + ";\n";

            json edit = {{"range",
                          {{"start", {{"line", insertLine}, {"character", 0}}},
                           {"end", {{"line", insertLine}, {"character", 0}}}}},
                         {"newText", insertText}};

            actions.push_back({{"title", "Add 'using " + typeName + " = " + target + ";'"},
                               {"kind", "quickfix"},
                               {"diagnostics", json::array({diag})},
                               {"edit", {{"changes", {{uri, json::array({edit})}}}}}});
        } else if (code == "unknown-function") {
            // Suggest adding an import statement
            std::string funcName = extractQuotedName(message);
            if (funcName.empty()) continue;

            // Infer a plausible .topo file from the function name
            // (take the first component before '.')
            std::string moduleName = funcName;
            auto dotPos = moduleName.find('.');
            if (dotPos != std::string::npos) {
                moduleName = moduleName.substr(0, dotPos);
            }

            int insertLine = findInsertionLine(docIt->second);
            std::string insertText = "import " + moduleName + ";\n";

            json edit = {{"range",
                          {{"start", {{"line", insertLine}, {"character", 0}}},
                           {"end", {{"line", insertLine}, {"character", 0}}}}},
                         {"newText", insertText}};

            actions.push_back({{"title", "Add 'import " + moduleName + ";'"},
                               {"kind", "quickfix"},
                               {"diagnostics", json::array({diag})},
                               {"edit", {{"changes", {{uri, json::array({edit})}}}}}});
        } else if (code == "non-canonical-syntax") {
            // Offer full-document replacement with canonical syntax
            auto cacheIt = analysisCache_.find(uri);
            if (cacheIt != analysisCache_.end() && !cacheIt->second.canonicalSource.empty()) {
                // Count lines in document to build full-document range
                const std::string& docText = docIt->second;
                int lastLine = 0;
                for (char c : docText) {
                    if (c == '\n') ++lastLine;
                }

                json textEdit = {
                    {"range",
                     {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", lastLine + 1}, {"character", 0}}}}},
                    {"newText", cacheIt->second.canonicalSource}};

                actions.push_back({{"title", "Convert to standard Topo syntax"},
                                   {"kind", "quickfix"},
                                   {"diagnostics", json::array({diag})},
                                   {"edit", {{"changes", {{uri, json::array({textEdit})}}}}}});
            }
        }
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", actions}};
}

int LSPServer::findInsertionLine(const std::string& source) const {
    // Find the end of the import/using block at the top of the file.
    // Returns a 0-based line number suitable for LSP insertion.
    std::istringstream stream(source);
    std::string line;
    int lastImportLine = 0; // insert at top by default
    int currentLine = 0;

    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            ++currentLine;
            continue;
        }
        std::string trimmed = line.substr(start);

        if (trimmed.rfind("import ", 0) == 0 || trimmed.rfind("using ", 0) == 0 ||
            trimmed.rfind("std::import", 0) == 0) {
            lastImportLine = currentLine + 1; // insert after this line
        }

        ++currentLine;
    }

    return lastImportLine;
}

std::string LSPServer::extractQuotedName(const std::string& message) {
    // Extract name from single quotes: 'name'
    auto start = message.find('\'');
    if (start == std::string::npos) return "";
    auto end = message.find('\'', start + 1);
    if (end == std::string::npos) return "";
    return message.substr(start + 1, end - start - 1);
}

// ============================================================================
// Semantic Tokens
// ============================================================================

json LSPServer::handleSemanticTokensFull(const json& id, const json& params) {
    auto uri = params["textDocument"]["uri"].get<std::string>();
    auto docIt = documents_.find(uri);
    auto cacheIt = analysisCache_.find(uri);

    if (docIt == documents_.end()) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"data", json::array()}}}};
    }

    const std::string& source = docIt->second;
    std::string filepath = uriToPath(uri);

    // Token type indices (must match legend order in handleInitialize)
    enum SemanticTokenType {
        TT_NAMESPACE = 0,
        TT_TYPE = 1,
        TT_CLASS = 2,
        TT_FUNCTION = 3,
        TT_VARIABLE = 4,
        TT_KEYWORD = 5,
        TT_COMMENT = 6,
        TT_STRING = 7,
        TT_NUMBER = 8,
        TT_OPERATOR = 9,
        TT_TYPEPARAM = 10,
        TT_PROPERTY = 11
    };

    // Token modifier bitmask
    constexpr int TM_DECLARATION = 1 << 0;
    // constexpr int TM_DEFINITION = 1 << 1;
    // constexpr int TM_READONLY   = 1 << 2;
    constexpr int TM_STATIC = 1 << 3;
    constexpr int TM_DEFAULTLIBRARY = 1 << 4; // stdlib bridging types

    // Lex the document to get all tokens
    DiagnosticEngine diag;
    Lexer lexer(source, filepath, diag);

    // Collect the SymbolTable if available
    const SymbolTable* symbols = nullptr;
    if (cacheIt != analysisCache_.end()) {
        symbols = &cacheIt->second.symbols;
    }

    // Build a simple name → qualified name lookup for identifier resolution
    // Maps simple names to their symbol kinds for quick classification
    struct SymbolInfo {
        int tokenType;
        int modifiers;
    };
    std::unordered_map<std::string, SymbolInfo> simpleNameMap;

    // Insert namespace names from AST (low priority — overridden by symbol table)
    if (cacheIt != analysisCache_.end() && cacheIt->second.ast) {
        for (const auto& decl : cacheIt->second.ast->declarations) {
            if (decl->kind == ASTKind::NamespaceDecl) {
                const auto& ns = static_cast<const NamespaceDecl&>(*decl);
                for (const auto& part : ns.path) {
                    simpleNameMap[part] = {TT_NAMESPACE, 0};
                }
            }
        }
    }

    if (symbols) {
        for (const auto& [qname, fn] : symbols->functions()) {
            int mods = 0;
            if (fn.isStatic) mods |= TM_STATIC;
            simpleNameMap[fn.simpleName] = {TT_FUNCTION, mods};
        }
        for (const auto& [name, ta] : symbols->typeAliases()) {
            simpleNameMap[ta.name] = {TT_TYPE, 0};
        }
        for (const auto& [qname, cls] : symbols->classSymbols()) {
            simpleNameMap[cls.simpleName] = {TT_CLASS, TM_DECLARATION};
        }
        for (const auto& [name, cs] : symbols->constraintSymbols()) {
            simpleNameMap[cs.simpleName] = {TT_TYPE, 0};
        }
        for (const auto& [qname, lb] : symbols->logicBlocks()) {
            simpleNameMap[lb.simpleName] = {TT_FUNCTION, 0};
        }
        for (const auto& imp : symbols->imports()) {
            if (!imp.typeName.empty()) {
                simpleNameMap[imp.typeName] = {TT_TYPE, 0};
            }
        }
        // Template parameter names (low priority — try_emplace avoids overriding concrete types)
        for (const auto& [qname, fn] : symbols->functions()) {
            for (const auto& tp : fn.templateParams) {
                simpleNameMap.try_emplace(tp.name, SymbolInfo{TT_TYPEPARAM, 0});
            }
        }
        for (const auto& [qname, cls] : symbols->classSymbols()) {
            for (const auto& tp : cls.templateParams) {
                simpleNameMap.try_emplace(tp.name, SymbolInfo{TT_TYPEPARAM, 0});
            }
        }
    }

    // Generate delta-encoded token data
    json data = json::array();
    int prevLine = 0;
    int prevCol = 0;
    TokenKind prevTokKind = TokenKind::Eof;

    Token tok = lexer.nextToken();
    while (tok.kind != TokenKind::Eof) {
        Token nextTok = lexer.nextToken();

        int tokenType = -1;
        int tokenModifiers = 0;
        int line = tok.location.line - 1;  // LSP is 0-based
        int col = tok.location.column - 1; // LSP is 0-based
        int length = static_cast<int>(tok.text.size());

        switch (tok.kind) {
        // Keywords — do NOT emit semantic tokens.
        // Let TextMate grammar handle keyword coloring (keyword.control.*
        // maps to purple in Dark+; semantic token "keyword" maps to blue).
        // This matches how clangd/rust-analyzer work: semantic tokens only
        // for identifiers that need symbol resolution.

        // import — context-sensitive:
        //   import types;      → skip (TextMate keyword.control.import → purple)
        //   std::import(...)   → emit function (yellow, like a built-in call)
        case TokenKind::KW_import:
            if (prevTokKind == TokenKind::ColonColon) {
                tokenType = TT_FUNCTION;
            }
            break;

        // Boolean literals
        case TokenKind::KW_true:
        case TokenKind::KW_false: tokenType = TT_NUMBER; break;

        // stdlib bridging type keywords are classified after the switch,
        // single-sourced from the stdlib table (see below).

        // Identifiers — resolve via symbol table
        case TokenKind::Identifier: {
            auto it = simpleNameMap.find(tok.text);
            if (it != simpleNameMap.end()) {
                tokenType = it->second.tokenType;
                tokenModifiers = it->second.modifiers;
            } else {
                tokenType = TT_VARIABLE;
            }
            break;
        }

        // Literals
        case TokenKind::IntegerLiteral: tokenType = TT_NUMBER; break;
        case TokenKind::StringLiteral: tokenType = TT_STRING; break;

        // Operators
        case TokenKind::Assign:
        case TokenKind::Arrow:
        case TokenKind::FatArrow:
        case TokenKind::ColonColon:
        case TokenKind::Dot:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Bang:
        case TokenKind::EqEq:
        case TokenKind::NotEq:
        case TokenKind::LessEq:
        case TokenKind::GreaterEq:
        case TokenKind::Percent:
        case TokenKind::Pipe:
        case TokenKind::Caret:
        case TokenKind::ShiftLeft:
        case TokenKind::ShiftRight:
        case TokenKind::AmpAmp:
        case TokenKind::PipePipe:
        case TokenKind::Amp:
        case TokenKind::Tilde:
        case TokenKind::Ellipsis: tokenType = TT_OPERATOR; break;

        // Delimiters — skip (not semantically meaningful)
        default: break;
        }

        // stdlib bridging type keywords render as `type` with the
        // `defaultLibrary` modifier so editors visually distinguish them
        // from user-defined types. Single-sourced from the stdlib table
        // (any width-extension/composite type is covered automatically).
        // Safe: literals/identifiers already set tokenType >= 0 above, so
        // only the reserved stdlib KW_* tokens reach this with text match.
        if (tokenType < 0 && topo::stdlib::isStdlibKeyword(tok.text)) {
            tokenType = TT_TYPE;
            tokenModifiers = TM_DEFAULTLIBRARY;
        }

        // Post-classification: qualifier context — identifier/keyword before :: → namespace
        if (tokenType >= 0 && nextTok.kind == TokenKind::ColonColon) {
            if (tok.kind == TokenKind::Identifier) {
                tokenType = TT_NAMESPACE;
            }
        }

        if (tokenType < 0) {
            prevTokKind = tok.kind;
            tok = nextTok;
            continue;
        }

        // Delta encoding: each token is (deltaLine, deltaStartChar, length, type, modifiers)
        int deltaLine = line - prevLine;
        int deltaStartChar = (deltaLine == 0) ? (col - prevCol) : col;

        data.push_back(deltaLine);
        data.push_back(deltaStartChar);
        data.push_back(length);
        data.push_back(tokenType);
        data.push_back(tokenModifiers);

        prevLine = line;
        prevCol = col;

        prevTokKind = tok.kind;
        tok = nextTok;
    }

    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"data", data}}}};
}

// ============================================================================
// URI helpers
// ============================================================================

std::string LSPServer::uriToPath(const std::string& uri) {
    // Handle file:///C:/path or file:///C%3A/path
    // Strip "file://" (7 chars) to preserve the leading '/' on Unix.
    // On Windows, paths start with a drive letter (e.g., /C:/...) so we
    // strip the extra leading '/' below.
    const std::string prefix = "file://";
    if (uri.substr(0, prefix.size()) == prefix) {
        std::string path = uri.substr(prefix.size());
        // Decode percent-encoded characters
        std::string decoded;
        for (size_t i = 0; i < path.size(); ++i) {
            // Only decode a %XX escape when both following chars are valid hex
            // digits; a malformed escape (e.g. %ZZ) must emit a literal '%'
            // rather than let std::stoi throw std::invalid_argument and unwind
            // out of nearly every handler.
            if (path[i] == '%' && i + 2 < path.size() &&
                std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
                auto hex = path.substr(i + 1, 2);
                decoded += static_cast<char>(std::stoi(hex, nullptr, 16));
                i += 2;
            } else {
                decoded += path[i];
            }
        }
#ifdef _WIN32
        // On Windows, strip leading '/' before drive letter: /C:/... → C:/...
        if (decoded.size() >= 3 && decoded[0] == '/' &&
            std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
            decoded = decoded.substr(1);
        }
#endif
        return decoded;
    }
    return uri;
}

std::string LSPServer::pathToUri(const std::string& path) {
    std::string normalized = path;
    // Convert backslashes to forward slashes
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }
    // On Unix, absolute paths start with '/' so "file://" + "/path" = "file:///path".
    // On Windows, prepend an extra '/' before the drive letter.
    if (!normalized.empty() && normalized[0] != '/') {
        return "file:///" + normalized;
    }
    return "file://" + normalized;
}

} // namespace topo::lsp
