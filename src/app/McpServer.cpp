#include "app/McpServer.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace mf {
namespace {

constexpr std::size_t kMaxRequestBytes = 16u * 1024u * 1024u;
constexpr const char* kSessionId = "materialforge-local";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

nlohmann::json RpcError(const nlohmann::json& id, int code, const std::string& message) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = {{"code", code}, {"message", message}};
    return response;
}

nlohmann::json ToolSchema(const std::string& type, const nlohmann::json& properties,
                          const nlohmann::json& required = nlohmann::json::array()) {
    return {{"type", type}, {"properties", properties}, {"required", required}};
}

std::string HttpHeaderValue(const std::string& headers, const std::string& name) {
    const std::string lowered = Lower(headers);
    const std::string needle = Lower(name) + ":";
    std::size_t position = lowered.find(needle);
    if (position == std::string::npos) return {};
    position += needle.size();
    while (position < headers.size() &&
           (headers[position] == ' ' || headers[position] == '\t')) {
        ++position;
    }
    const std::size_t end = headers.find("\r\n", position);
    return headers.substr(position, end == std::string::npos ? std::string::npos
                                                             : end - position);
}

#if defined(_WIN32)
bool SendAll(SOCKET socket, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(
            data.size() - offset, static_cast<std::size_t>(1u << 20)));
        const int sent = ::send(socket, data.data() + offset, chunk, 0);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool SendHttp(SOCKET socket, int status, const std::string& reason,
              const std::string& body, const std::string& contentType) {
    std::ostringstream stream;
    stream << "HTTP/1.1 " << status << " " << reason << "\r\n";
    stream << "Content-Type: " << contentType << "\r\n";
    stream << "Content-Length: " << body.size() << "\r\n";
    stream << "Connection: close\r\n";
    stream << "Access-Control-Allow-Origin: *\r\n";
    stream << "Access-Control-Allow-Headers: content-type, mcp-protocol-version, mcp-session-id\r\n";
    stream << "Access-Control-Allow-Methods: POST, GET, OPTIONS, DELETE\r\n";
    stream << "Access-Control-Expose-Headers: mcp-session-id\r\n";
    stream << "Mcp-Session-Id: " << kSessionId << "\r\n";
    stream << "\r\n";
    stream << body;
    return SendAll(socket, stream.str());
}

bool ReceiveRequest(SOCKET socket, std::string* method, std::string* path,
                    std::string* body, std::string* error) {
    std::string data;
    char buffer[8192];
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int received = ::recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            if (error) *error = "Connection closed before HTTP headers were complete.";
            return false;
        }
        data.append(buffer, static_cast<std::size_t>(received));
        if (data.size() > kMaxRequestBytes) {
            if (error) *error = "HTTP request is too large.";
            return false;
        }
        headerEnd = data.find("\r\n\r\n");
    }
    const std::string headers = data.substr(0, headerEnd);
    const std::size_t requestLineEnd = headers.find("\r\n");
    const std::string requestLine = headers.substr(0, requestLineEnd);
    std::istringstream requestStream(requestLine);
    std::string version;
    requestStream >> *method >> *path >> version;
    if (method->empty() || path->empty()) {
        if (error) *error = "Malformed HTTP request line.";
        return false;
    }
    std::size_t contentLength = 0;
    const std::string lengthText = HttpHeaderValue(headers, "Content-Length");
    if (!lengthText.empty()) {
        try {
            contentLength = static_cast<std::size_t>(std::stoull(lengthText));
        } catch (...) {
            if (error) *error = "Invalid Content-Length.";
            return false;
        }
    }
    if (contentLength > kMaxRequestBytes) {
        if (error) *error = "HTTP body is too large.";
        return false;
    }
    body->clear();
    const std::size_t bodyStart = headerEnd + 4;
    if (data.size() > bodyStart) {
        body->assign(data.begin() + static_cast<std::ptrdiff_t>(bodyStart), data.end());
    }
    while (body->size() < contentLength) {
        const int received = ::recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        body->append(buffer, static_cast<std::size_t>(received));
        if (body->size() > kMaxRequestBytes) {
            if (error) *error = "HTTP body is too large.";
            return false;
        }
    }
    if (body->size() > contentLength) body->resize(contentLength);
    if (error) error->clear();
    return true;
}
#endif

} // namespace

McpServer::~McpServer() {
    Stop();
}

bool McpServer::Start(std::uint16_t port, ToolHandler toolHandler, LogHandler logHandler,
                      std::string* error) {
    Stop();
    if (!toolHandler) {
        if (error) *error = "MCP tool handler is missing.";
        return false;
    }
#if !defined(_WIN32)
    if (error) *error = "The built-in MCP server is only available on Windows in this build.";
    (void)port;
    return false;
#else
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        if (error) *error = "WSAStartup failed.";
        return false;
    }
    SOCKET listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        WSACleanup();
        if (error) *error = "Could not create the MCP listen socket.";
        return false;
    }
    BOOL reuse = TRUE;
    ::setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        const int code = WSAGetLastError();
        ::closesocket(listenSocket);
        WSACleanup();
        if (error) *error = "Could not bind MCP port " + std::to_string(port) + " (error " +
                            std::to_string(code) + ").";
        return false;
    }
    if (::listen(listenSocket, 8) == SOCKET_ERROR) {
        ::closesocket(listenSocket);
        WSACleanup();
        if (error) *error = "Could not listen on the MCP socket.";
        return false;
    }
    u_long nonBlocking = 1;
    ::ioctlsocket(listenSocket, FIONBIO, &nonBlocking);
    m_ListenSocket = static_cast<std::uintptr_t>(listenSocket);
    m_Port = port;
    m_ToolHandler = std::move(toolHandler);
    m_LogHandler = std::move(logHandler);
    SetLastError({});
    m_Running = true;
    m_Thread = std::thread([this] { ThreadMain(); });
    Log(0, "MCP server listening on " + Endpoint());
    if (error) error->clear();
    return true;
#endif
}

void McpServer::Stop() {
    if (!m_Running.exchange(false)) {
#if defined(_WIN32)
        if (m_ListenSocket != static_cast<std::uintptr_t>(~0ull)) {
            ::closesocket(static_cast<SOCKET>(m_ListenSocket));
            m_ListenSocket = static_cast<std::uintptr_t>(~0ull);
            WSACleanup();
        }
#endif
        return;
    }
#if defined(_WIN32)
    if (m_ListenSocket != static_cast<std::uintptr_t>(~0ull)) {
        ::closesocket(static_cast<SOCKET>(m_ListenSocket));
        m_ListenSocket = static_cast<std::uintptr_t>(~0ull);
    }
#endif
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
#if defined(_WIN32)
    WSACleanup();
#endif
    std::deque<std::shared_ptr<PendingCall>> pending;
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        pending.swap(m_Queue);
    }
    for (const auto& call : pending) {
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->error = "MCP server stopped.";
            call->done = true;
        }
        call->ready.notify_all();
    }
    m_ToolHandler = nullptr;
    m_LogHandler = nullptr;
    Log(0, "MCP server stopped.");
}

std::string McpServer::LastError() const {
    std::lock_guard<std::mutex> lock(m_ErrorMutex);
    return m_LastError;
}

std::string McpServer::Endpoint() const {
    return "http://127.0.0.1:" + std::to_string(m_Port.load()) + "/mcp";
}

void McpServer::SetLastError(const std::string& error) {
    std::lock_guard<std::mutex> lock(m_ErrorMutex);
    m_LastError = error;
}

void McpServer::Log(int level, const std::string& message) {
    if (m_LogHandler) m_LogHandler(level, message);
}

nlohmann::json McpServer::DescribeTools() {
    using nlohmann::json;
    json tools = json::array();

    tools.push_back({
        {"name", "mf_status"},
        {"description", "Return MaterialForge version, open project, active document and preview backend."},
        {"inputSchema", ToolSchema("object", json::object())},
    });
    tools.push_back({
        {"name", "mf_list_materials"},
        {"description", "List material and blueprint-function JSON assets in the open project."},
        {"inputSchema", ToolSchema("object", {
            {"folder", {{"type", "string"}, {"description", "Optional project-relative folder."}}},
        })},
    });
    tools.push_back({
        {"name", "mf_create_material"},
        {"description", "Create a new material JSON in the open project. Optionally provide a graph document or choose the default starter graph."},
        {"inputSchema", ToolSchema("object", {
            {"name", {{"type", "string"}, {"description", "Material name without .json."}}},
            {"folder", {{"type", "string"}, {"description", "Project-relative destination folder."}}},
            {"template", {{"type", "string"}, {"enum", json::array({"default", "empty"})}}},
            {"graph", {{"type", "object"}, {"description", "Optional materialforge.ue5.material graph document."}}},
        }, json::array({"name"}))},
    });
    tools.push_back({
        {"name", "mf_get_graph"},
        {"description", "Read a material graph as a materialforge.ue5.material JSON document."},
        {"inputSchema", ToolSchema("object", {
            {"path", {{"type", "string"}, {"description", "Project-relative material JSON path."}}},
        }, json::array({"path"}))},
    });
    tools.push_back({
        {"name", "mf_set_graph"},
        {"description", "Replace a material graph with a JSON document."},
        {"inputSchema", ToolSchema("object", {
            {"path", {{"type", "string"}}},
            {"graph", {{"type", "object"}}},
        }, json::array({"path", "graph"}))},
    });
    tools.push_back({
        {"name", "mf_add_node"},
        {"description", "Add a MaterialForge node to a material graph."},
        {"inputSchema", ToolSchema("object", {
            {"path", {{"type", "string"}}},
            {"type", {{"type", "string"}, {"description", "Node type such as Constant3Vector, Multiply or TextureSample."}}},
            {"x", {{"type", "number"}}},
            {"y", {{"type", "number"}}},
            {"properties", {{"type", "object"}, {"description", "Optional property values keyed by property name."}}},
        }, json::array({"path", "type", "x", "y"}))},
    });
    tools.push_back({
        {"name", "mf_connect"},
        {"description", "Connect an output pin to an input pin in a material graph."},
        {"inputSchema", ToolSchema("object", {
            {"path", {{"type", "string"}}},
            {"fromNode", {{"type", "integer"}}},
            {"fromPin", {{"type", "string"}}},
            {"toNode", {{"type", "integer"}}},
            {"toPin", {{"type", "string"}}},
        }, json::array({"path", "fromNode", "fromPin", "toNode", "toPin"}))},
    });
    tools.push_back({
        {"name", "mf_import_ue5_text"},
        {"description", "Import Unreal Engine material-node clipboard text into a material graph."},
        {"inputSchema", ToolSchema("object", {
            {"path", {{"type", "string"}}},
            {"text", {{"type", "string"}}},
        }, json::array({"path", "text"}))},
    });
    return tools;
}

nlohmann::json McpServer::CallTool(const std::string& tool, const nlohmann::json& arguments) {
    if (!m_ToolHandler) {
        return {{"ok", false}, {"error", "MCP server is not running."}};
    }
    auto pending = std::make_shared<PendingCall>();
    pending->tool = tool;
    pending->arguments = arguments;
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_Queue.push_back(pending);
    }
    std::unique_lock<std::mutex> lock(pending->mutex);
    if (!pending->ready.wait_for(lock, std::chrono::seconds(45),
                                 [&] { return pending->done; })) {
        return {{"ok", false}, {"error", "Tool call timed out waiting for the editor."}};
    }
    if (!pending->error.empty()) {
        return {{"ok", false}, {"error", pending->error}};
    }
    return pending->result;
}

void McpServer::ProcessPending() {
    std::deque<std::shared_ptr<PendingCall>> calls;
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        calls.swap(m_Queue);
    }
    for (const auto& call : calls) {
        nlohmann::json result;
        std::string error;
        try {
            if (m_ToolHandler) {
                result = m_ToolHandler(call->tool, call->arguments);
            } else {
                error = "MCP server is not running.";
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Unknown MCP tool error.";
        }
        {
            std::lock_guard<std::mutex> lock(call->mutex);
            call->result = std::move(result);
            call->error = std::move(error);
            call->done = true;
        }
        call->ready.notify_all();
    }
}

nlohmann::json McpServer::HandleRpc(const nlohmann::json& request) {
    using nlohmann::json;
    if (!request.is_object()) {
        return RpcError(nullptr, -32600, "Invalid Request");
    }
    const json id = request.contains("id") ? request["id"] : json(nullptr);
    const std::string method = request.value("method", std::string());
    if (method.empty()) {
        return RpcError(id, -32600, "Missing method.");
    }
    if (method == "initialize") {
        std::string protocolVersion = "2024-11-05";
        if (request.contains("params") && request["params"].is_object()) {
            const auto& params = request["params"];
            if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
                protocolVersion = params["protocolVersion"].get<std::string>();
            }
        }
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result",
             {{"protocolVersion", protocolVersion},
              {"capabilities", {{"tools", {{"listChanged", false}}}}},
              {"serverInfo", {{"name", "MaterialForge"}, {"version", MF_VERSION_STRING}}}}},
        };
    }
    if (method == "notifications/initialized" || method.rfind("notifications/", 0) == 0) {
        return json();
    }
    if (method == "ping") {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
    }
    if (method == "tools/list") {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", DescribeTools()}}}};
    }
    if (method == "resources/list") {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"resources", json::array()}}}};
    }
    if (method == "prompts/list") {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"prompts", json::array()}}}};
    }
    if (method == "tools/call") {
        if (!request.contains("params") || !request["params"].is_object()) {
            return RpcError(id, -32602, "tools/call requires params.");
        }
        const json& params = request["params"];
        const std::string tool = params.value("name", std::string());
        const json arguments = params.contains("arguments") && params["arguments"].is_object()
                                   ? params["arguments"]
                                   : json::object();
        if (tool.empty()) {
            return RpcError(id, -32602, "Missing tool name.");
        }
        const json toolResult = CallTool(tool, arguments);
        const bool failed = toolResult.is_object() && toolResult.contains("ok") &&
                            toolResult["ok"].is_boolean() && !toolResult["ok"].get<bool>();
        std::string text;
        if (failed && toolResult.contains("error")) {
            text = toolResult["error"].is_string() ? toolResult["error"].get<std::string>()
                                                   : toolResult["error"].dump(2);
        } else if (toolResult.contains("data")) {
            text = toolResult["data"].dump(2);
        } else {
            text = toolResult.dump(2);
        }
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result",
             {{"content", json::array({{{"type", "text"}, {"text", text}}})},
              {"isError", failed}}},
        };
    }
    return RpcError(id, -32601, "Method not found: " + method);
}

void McpServer::ThreadMain() {
#if defined(_WIN32)
    const SOCKET listenSocket = static_cast<SOCKET>(m_ListenSocket);
    while (m_Running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
        timeval timeout{};
        timeout.tv_usec = 200000;
        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
        if (!m_Running.load()) break;
        if (ready <= 0) continue;
        sockaddr_in clientAddress{};
        int addressSize = sizeof(clientAddress);
        SOCKET client = ::accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddress),
                                 &addressSize);
        if (client == INVALID_SOCKET) continue;
        u_long blocking = 0;
        ::ioctlsocket(client, FIONBIO, &blocking);
        DWORD timeoutMs = 5000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        std::string method;
        std::string path;
        std::string body;
        std::string httpError;
        if (!ReceiveRequest(client, &method, &path, &body, &httpError)) {
            SendHttp(client, 400, "Bad Request", httpError, "text/plain");
            ::closesocket(client);
            continue;
        }
        if (method == "OPTIONS") {
            SendHttp(client, 204, "No Content", {}, "text/plain");
            ::closesocket(client);
            continue;
        }
        if (method == "GET" && (path == "/mcp" || path == "/")) {
            const std::string text =
                "MaterialForge MCP endpoint. POST JSON-RPC 2.0 requests to " + Endpoint() + ".";
            SendHttp(client, 200, "OK", text, "text/plain");
            ::closesocket(client);
            continue;
        }
        if (method == "DELETE") {
            SendHttp(client, 204, "No Content", {}, "text/plain");
            ::closesocket(client);
            continue;
        }
        if (method != "POST") {
            SendHttp(client, 405, "Method Not Allowed", "Use POST for MCP requests.",
                     "text/plain");
            ::closesocket(client);
            continue;
        }
        nlohmann::json request;
        try {
            request = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
        } catch (const std::exception& exception) {
            nlohmann::json response =
                RpcError(nullptr, -32700, std::string("Parse error: ") + exception.what());
            SendHttp(client, 200, "OK", response.dump(), "application/json");
            ::closesocket(client);
            continue;
        }

        nlohmann::json response;
        if (request.is_array()) {
            response = nlohmann::json::array();
            for (const nlohmann::json& item : request) {
                try {
                    const nlohmann::json one = HandleRpc(item);
                    if (!one.is_null() && !one.empty()) response.push_back(one);
                } catch (const std::exception& exception) {
                    response.push_back(RpcError(item.is_object() && item.contains("id")
                                                    ? item["id"]
                                                    : nlohmann::json(nullptr),
                                                -32603,
                                                std::string("Internal error: ") +
                                                    exception.what()));
                }
            }
            if (response.empty()) {
                SendHttp(client, 202, "Accepted", {}, "application/json");
            } else {
                SendHttp(client, 200, "OK", response.dump(), "application/json");
            }
        } else if (request.is_object() && request.contains("id")) {
            try {
                response = HandleRpc(request);
            } catch (const std::exception& exception) {
                response = RpcError(request["id"], -32603,
                                    std::string("Internal error: ") + exception.what());
            }
            if (!response.is_null() && !response.empty()) {
                SendHttp(client, 200, "OK", response.dump(), "application/json");
            } else {
                SendHttp(client, 202, "Accepted", {}, "application/json");
            }
        } else {
            // Notifications do not receive a JSON-RPC response.
            try {
                HandleRpc(request);
            } catch (...) {
            }
            SendHttp(client, 202, "Accepted", {}, "application/json");
        }
        ::closesocket(client);
    }
#endif
}

} // namespace mf
