// LspProtocolFixture.h — shared test harness for protocol-level LSP tests.
//
// Spawns the real topo-lsp binary as a subprocess, exchanges JSON-RPC messages
// over stdin/stdout pipes (real LSP base protocol, no mocks), and exposes a
// small API for request/response and notification messaging with timeouts.
//
// Transport: topo::platform::PipedProcess for stdin/stdout framing.
// Reading:   a background reader thread accumulates framed messages and
//            buckets them into responses (keyed by id) and notifications.
// Timeouts:  every read path honours a std::chrono deadline to avoid ever
//            blocking the test thread indefinitely.
#ifndef TOPO_LSP_TEST_PROTOCOL_FIXTURE_H
#define TOPO_LSP_TEST_PROTOCOL_FIXTURE_H

#include "topo/Platform/Process.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace topo::lsp::test {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::milliseconds;

// Client that drives a real topo-lsp subprocess over JSON-RPC pipe transport.
//
// One fixture instance per test (or per test fixture SetUp). The destructor
// shuts down the process gracefully, falling back to forceful termination.
class LspProtocolClient {
public:
    // Default per-read timeout. Tests may override per-call.
    static constexpr Millis kDefaultTimeout{5000};

    // Launch the topo-lsp subprocess at `exePath`. Throws std::runtime_error
    // on failure so GTest ASSERT macros at the call site report the cause.
    explicit LspProtocolClient(const std::string& exePath) {
        if (!process_.start(exePath, {})) {
            throw std::runtime_error("failed to spawn topo-lsp at " + exePath);
        }
        stopping_ = false;
        readerThread_ = std::thread(&LspProtocolClient::readerLoop, this);
    }

    LspProtocolClient(const LspProtocolClient&) = delete;
    LspProtocolClient& operator=(const LspProtocolClient&) = delete;

    ~LspProtocolClient() {
        // Best-effort graceful shutdown. Any exception is swallowed so we
        // never throw from the destructor.
        try {
            if (!terminated_) {
                // Only send shutdown/exit if we believe the loop is still alive.
                if (process_.isRunning() && !readerExited_) {
                    json shutdownMsg = {
                        {"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "shutdown"}, {"params", json::object()}};
                    (void)writeMessageInternal(shutdownMsg);
                    // We don't wait for the response — just move on to exit.
                    json exitMsg = {{"jsonrpc", "2.0"}, {"method", "exit"}, {"params", json::object()}};
                    (void)writeMessageInternal(exitMsg);
                }
            }
        } catch (...) {
        }
        stopping_ = true;
        process_.stop(3000);
        if (readerThread_.joinable()) {
            readerThread_.join();
        }
    }

    // Send a request and wait for the response with a matching id.
    // Returns the full response object (jsonrpc/id/result-or-error). The caller
    // inspects `response["result"]` or `response["error"]`.
    // On timeout returns std::nullopt.
    std::optional<json> sendRequest(const std::string& method,
                                    const json& params,
                                    Millis timeout = kDefaultTimeout) {
        int id;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            id = nextId_++;
        }
        json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        if (!writeMessageInternal(msg)) {
            return std::nullopt;
        }
        return waitForResponse(id, timeout);
    }

    // Fire a notification (no id, no response expected).
    bool sendNotification(const std::string& method, const json& params) {
        json msg = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
        return writeMessageInternal(msg);
    }

    // Pull the next server-initiated notification (e.g. textDocument/publishDiagnostics)
    // from the queue. Returns std::nullopt on timeout.
    std::optional<json> nextNotification(Millis timeout = kDefaultTimeout) {
        std::unique_lock<std::mutex> lock(responseMutex_);
        auto deadline = Clock::now() + timeout;
        while (notifications_.empty()) {
            if (readerExited_ && notifications_.empty()) return std::nullopt;
            if (responseCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return std::nullopt;
            }
        }
        json n = std::move(notifications_.front());
        notifications_.pop_front();
        return n;
    }

    // Drain all currently-queued notifications. Safe to call without blocking.
    std::vector<json> drainNotifications() {
        std::lock_guard<std::mutex> lock(responseMutex_);
        std::vector<json> out;
        out.reserve(notifications_.size());
        while (!notifications_.empty()) {
            out.push_back(std::move(notifications_.front()));
            notifications_.pop_front();
        }
        return out;
    }

    // Convenience: send an LSP `initialize` request with a given rootUri
    // (pass nullptr-equivalent json for no workspace). Returns the result.
    std::optional<json> initialize(const json& rootUri = nullptr,
                                   Millis timeout = kDefaultTimeout) {
        json params = {{"rootUri", rootUri}, {"capabilities", json::object()}};
        return sendRequest("initialize", params, timeout);
    }

    // Convenience: send the `initialized` notification.
    void initialized() {
        sendNotification("initialized", json::object());
    }

    // Convenience: `shutdown` request.
    std::optional<json> shutdown(Millis timeout = kDefaultTimeout) {
        return sendRequest("shutdown", json::object(), timeout);
    }

    // Convenience: `exit` notification. Marks the client as terminated so the
    // destructor won't redundantly send shutdown/exit.
    void exit() {
        sendNotification("exit", json::object());
        terminated_ = true;
    }

    // Poll for up to `timeout` ms waiting for the subprocess to exit. Returns
    // true if the process terminated within the window.
    bool waitForTermination(Millis timeout) {
        auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (!process_.isRunning()) return true;
            std::this_thread::sleep_for(Millis{50});
        }
        return !process_.isRunning();
    }

    // Wait for the subprocess to exit and return its exit code. Call after
    // exit() or shutdown()+exit(). Returns -1 if the process did not
    // terminate in time or if the OS could not report the status. Internally
    // reaps the child via process_.stop() so the exit status is captured.
    int waitForExit(Millis timeout = Millis{5000}) {
        (void)waitForTermination(timeout);
        process_.stop(100);
        terminated_ = true;
        return process_.exitCode();
    }

    // Send didOpen for a .topo document with the given uri and text.
    // Also drains any publishDiagnostics notifications the server emits in
    // response so the notification queue stays clean for tests that care.
    void didOpen(const std::string& uri, const std::string& text, int version = 1) {
        json params = {
            {"textDocument",
             {{"uri", uri}, {"languageId", "topo"}, {"version", version}, {"text", text}}}};
        sendNotification("textDocument/didOpen", params);
        // Give the server a moment to emit diagnostics, then drain.
        std::this_thread::sleep_for(Millis{50});
        (void)drainNotifications();
    }

    // Send didClose for a .topo document.
    void didClose(const std::string& uri) {
        json params = {{"textDocument", {{"uri", uri}}}};
        sendNotification("textDocument/didClose", params);
        std::this_thread::sleep_for(Millis{50});
        (void)drainNotifications();
    }

    // Send a full-document didChange (no range).
    void didChangeFull(const std::string& uri, const std::string& newText, int version = 2) {
        json change = {{"text", newText}};
        json params = {
            {"textDocument", {{"uri", uri}, {"version", version}}},
            {"contentChanges", json::array({change})}};
        sendNotification("textDocument/didChange", params);
        std::this_thread::sleep_for(Millis{50});
        (void)drainNotifications();
    }

    // Send a range-based (incremental) didChange.
    void didChangeRange(const std::string& uri,
                        int startLine, int startChar,
                        int endLine, int endChar,
                        const std::string& newText,
                        int version = 2) {
        json change = {
            {"range",
             {{"start", {{"line", startLine}, {"character", startChar}}},
              {"end", {{"line", endLine}, {"character", endChar}}}}},
            {"text", newText}};
        json params = {
            {"textDocument", {{"uri", uri}, {"version", version}}},
            {"contentChanges", json::array({change})}};
        sendNotification("textDocument/didChange", params);
        std::this_thread::sleep_for(Millis{50});
        (void)drainNotifications();
    }

    bool isRunning() const { return process_.isRunning(); }

private:
    // ---------- transport ----------

    bool writeMessageInternal(const json& msg) {
        std::string body = msg.dump();
        std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        std::string data = header + body;
        std::lock_guard<std::mutex> lock(writeMutex_);
        return process_.write(data.c_str(), data.size());
    }

    // Background-thread read of a framed LSP message. Blocks on readByte().
    // Returns std::nullopt when the subprocess closes its stdout.
    std::optional<json> readFramedMessage() {
        int contentLength = -1;
        while (true) {
            std::string line;
            while (true) {
                int ch = process_.readByte();
                if (ch < 0) return std::nullopt;
                if (ch == '\r') {
                    int next = process_.readByte();
                    if (next == '\n') break;
                    line += static_cast<char>(ch);
                    if (next >= 0) line += static_cast<char>(next);
                } else {
                    line += static_cast<char>(ch);
                }
            }
            if (line.empty()) break; // end of headers
            const std::string prefix = "Content-Length: ";
            if (line.substr(0, prefix.size()) == prefix) {
                try {
                    contentLength = std::stoi(line.substr(prefix.size()));
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
        if (contentLength <= 0) return std::nullopt;
        std::string body(static_cast<size_t>(contentLength), '\0');
        size_t read = 0;
        while (read < body.size()) {
            size_t n = process_.read(&body[read], body.size() - read);
            if (n == 0) return std::nullopt;
            read += n;
        }
        try {
            return json::parse(body);
        } catch (...) {
            return std::nullopt;
        }
    }

    void readerLoop() {
        while (!stopping_) {
            auto msg = readFramedMessage();
            if (!msg) break;
            // Route by id: responses go to pendingResponses_, notifications to queue.
            bool hasId = msg->contains("id") && !(*msg)["id"].is_null();
            {
                std::lock_guard<std::mutex> lock(responseMutex_);
                if (hasId) {
                    int id = (*msg)["id"].get<int>();
                    pendingResponses_[id] = std::move(*msg);
                } else {
                    notifications_.push_back(std::move(*msg));
                }
            }
            responseCv_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(responseMutex_);
            readerExited_ = true;
        }
        responseCv_.notify_all();
    }

    std::optional<json> waitForResponse(int id, Millis timeout) {
        std::unique_lock<std::mutex> lock(responseMutex_);
        auto deadline = Clock::now() + timeout;
        while (pendingResponses_.find(id) == pendingResponses_.end()) {
            if (readerExited_) {
                return std::nullopt;
            }
            if (responseCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                pendingResponses_.erase(id);
                return std::nullopt;
            }
        }
        auto it = pendingResponses_.find(id);
        json response = std::move(it->second);
        pendingResponses_.erase(it);
        return response;
    }

    topo::platform::PipedProcess process_;
    std::thread readerThread_;
    std::atomic<bool> stopping_{false};

    // Lifecycle flags observed from the client side.
    std::atomic<bool> terminated_{false};

    std::mutex writeMutex_;
    int nextId_ = 1;

    std::mutex responseMutex_;
    std::condition_variable responseCv_;
    std::unordered_map<int, json> pendingResponses_;
    std::deque<json> notifications_;
    // Set by the reader thread on exit, read by the destructor WITHOUT holding
    // responseMutex_ (line ~65) as well as by the mutex-guarded wait paths.
    // Making it atomic removes the destructor data race while leaving the
    // condition-variable predicate reads correct.
    std::atomic<bool> readerExited_{false};
};

// Helper: given a LSP response object, return true iff it contains a well-formed
// `result` field and does NOT contain an `error` field.
inline bool responseHasResult(const std::optional<json>& resp) {
    if (!resp) return false;
    return resp->contains("result") && !resp->contains("error");
}

} // namespace topo::lsp::test

#endif // TOPO_LSP_TEST_PROTOCOL_FIXTURE_H
