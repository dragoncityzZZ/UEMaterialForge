#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mf {

// Small local-only MCP (Model Context Protocol) server. It exposes a
// Streamable-HTTP style endpoint at /mcp and forwards tool calls to the
// application's main thread so tools can safely touch the graph and project.
class McpServer {
public:
    using ToolHandler =
        std::function<nlohmann::json(const std::string& tool, const nlohmann::json& args)>;
    using LogHandler = std::function<void(int level, const std::string& message)>;

    McpServer() = default;
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    bool Start(std::uint16_t port, ToolHandler toolHandler, LogHandler logHandler,
               std::string* error = nullptr);
    void Stop();
    bool IsRunning() const { return m_Running.load(); }
    std::uint16_t Port() const { return m_Port; }
    std::string LastError() const;
    std::string Endpoint() const;

    // Called from the main thread once per frame. Executes tool calls that the
    // HTTP worker queued and wakes the waiting request.
    void ProcessPending();

    static nlohmann::json DescribeTools();

private:
    struct PendingCall {
        std::string tool;
        nlohmann::json arguments;
        nlohmann::json result;
        std::string error;
        bool done = false;
        std::mutex mutex;
        std::condition_variable ready;
    };

    void ThreadMain();
    nlohmann::json HandleRpc(const nlohmann::json& request);
    nlohmann::json CallTool(const std::string& tool, const nlohmann::json& arguments);
    void Log(int level, const std::string& message);
    void SetLastError(const std::string& error);

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<std::uint16_t> m_Port{0};
    ToolHandler m_ToolHandler;
    LogHandler m_LogHandler;
    std::mutex m_QueueMutex;
    std::deque<std::shared_ptr<PendingCall>> m_Queue;
    mutable std::mutex m_ErrorMutex;
    std::string m_LastError;
#if defined(_WIN32)
    std::uintptr_t m_ListenSocket = static_cast<std::uintptr_t>(~0ull);
#else
    int m_ListenSocket = -1;
#endif
};

} // namespace mf
