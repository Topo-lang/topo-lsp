#ifndef TOPO_LSP_LSPSERVER_H
#define TOPO_LSP_LSPSERVER_H

#include "topo/LSP/LSPBridge.h"
#include "topo/AST/ASTNode.h"
#include "topo/Basic/Diagnostic.h"
#include "topo/Basic/HostLanguage.h"
#include "topo/Sema/SymbolTable.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace topo::lsp {

using json = nlohmann::json;

struct AnalysisResult {
    std::unique_ptr<TopoFile> ast;
    SymbolTable symbols;
    std::vector<Diagnostic> diagnostics;
    std::string canonicalSource; // empty if already canonical
};

// --- ProjectConfig: parsed from Topo.toml ---

struct ProjectConfig {
    std::string topoRoot;
    std::vector<std::string> sources;
    std::vector<std::string> includeDirs;
    HostLanguage language = HostLanguage::Cpp;
};

// --- HostFunctionIndex: maps qualified host function names to source locations ---

struct HostSymbolLocation {
    std::string file;
    int line;
};

class HostFunctionIndex {
public:
    void scanFile(const std::string& filepath);
    void scanCompileCommands(const std::string& buildDir);
    const HostSymbolLocation* find(const std::string& qualifiedName) const;
    bool empty() const { return index_.empty(); }

private:
    void scanCppFile(const std::string& filepath);
    void scanRustFile(const std::string& filepath);
    void scanJavaFile(const std::string& filepath);
    void scanPythonFile(const std::string& filepath);

    std::unordered_map<std::string, HostSymbolLocation> index_;
};

// Backward-compatible type aliases
using CppFunctionIndex = HostFunctionIndex;
using CppSymbolLocation = HostSymbolLocation;

// --- LSP Server ---

class LSPServer {
public:
    // Process a JSON-RPC message and return an optional response.
    // Returns std::nullopt for notifications that need no response.
    std::optional<json> handleMessage(const json& msg);

    // Check if the server has been asked to shut down.
    bool shouldExit() const { return exitRequested_; }

    // LSP 3.17 §3.17: exit code is 0 when the client sent `shutdown` before
    // `exit`, and non-zero when `exit` arrives without a prior `shutdown`.
    int exitCode() const { return shutdownRequested_ ? 0 : 1; }

    // Retrieve pending notifications (e.g. publishDiagnostics).
    std::vector<json> takePendingNotifications();

private:
    // --- Lifecycle ---
    json handleInitialize(const json& id, const json& params);
    json handleShutdown(const json& id);
    void handleExit();

    // --- Document sync ---
    void handleDidOpen(const json& params);
    void handleDidChange(const json& params);
    void handleDidClose(const json& params);

    // --- Language features ---
    json handleFormatting(const json& id, const json& params);
    json handleRangeFormatting(const json& id, const json& params);
    json handleHover(const json& id, const json& params);
    json handleDefinition(const json& id, const json& params);
    json handleTomlDefinition(const json& id, const std::string& uri, int line, int col);
    json handleReferences(const json& id, const json& params);
    json handleCompletion(const json& id, const json& params);
    json handleDocumentSymbol(const json& id, const json& params);
    json handleCodeAction(const json& id, const json& params);
    json handleSemanticTokensFull(const json& id, const json& params);

    // --- Internal ---
    void analyzeDocument(const std::string& uri);
    void publishDiagnostics(const std::string& uri);
    void validateTopoToml(const std::string& uri, const std::string& filePath);

    // Apply a single incremental content change to a document.
    void applyContentChange(std::string& doc, const json& change);

    // Quick Fix helpers
    int findInsertionLine(const std::string& source) const;
    static std::string extractQuotedName(const std::string& message);

    // Find the identifier at the given line/column in the source text.
    std::string identifierAtPosition(const std::string& source, int line, int column) const;

    // Scan identifier length at position in source for diagnostic range.
    int identifierLengthAt(const std::string& source, int line, int column) const;

    // Length (in characters) of the 1-based `line` in `source`, or -1 when the
    // line does not exist. Used to clamp diagnostic end columns to the line.
    int lineLengthAt(const std::string& source, int line) const;

    // Build qualified name candidates from an identifier.
    std::vector<std::string> qualifiedCandidates(const std::string& uri, const std::string& identifier) const;

    // Find which namespace scope contains the given line.
    std::string findNamespaceAtLine(const TopoFile& root, int line) const;

    // Check if the given position is inside a fn { ... } block.
    bool isInsideFnBlock(const std::string& source, int line, int col) const;

    // Collect document symbols from AST.
    json collectSymbols(const TopoFile& root) const;
    json collectNamespaceSymbols(const NamespaceDecl& ns, const std::string& parentPath) const;

    // --- Project config & host function index ---
    void loadProjectConfig(const std::string& rootPath);
    void discoverProjectConfig(const std::string& filepath);

    // URI helpers
    static std::string uriToPath(const std::string& uri);
    static std::string pathToUri(const std::string& path);

    // State
    std::unordered_map<std::string, std::string> documents_;
    std::unordered_map<std::string, AnalysisResult> analysisCache_;
    std::vector<json> pendingNotifications_;
    bool initialized_ = false;
    bool shutdownRequested_ = false;
    bool exitRequested_ = false;

    // Workspace root (from initialize params)
    std::string workspaceRoot_;

    // Project configuration (from Topo.toml)
    ProjectConfig projectConfig_;

    // Host function index
    HostFunctionIndex hostIndex_;

    // Language-specific LSP bridge (created via plugin)
    std::unique_ptr<LSPBridge> bridge_;
};

} // namespace topo::lsp

#endif // TOPO_LSP_LSPSERVER_H
