// SPDX-License-Identifier: Apache-2.0
// forge3d-mcp: an MCP server (stdio) bridging to the editor's JSON-RPC server
// (phases.md P5.3). See docs/guides/mcp.md.
//
// It reads the editor's {port, token} file and connects; if no editor is running
// (or --spawn-editor), it starts a headless one and stops it on exit. MCP tools are
// generated from the editor's operations (ops.list: "entity.create" -> tool
// "entity_create" with the operation's JSON Schema), plus screenshot, undo, redo,
// scene_query, entity_get, terrain_sample and asset_search. Resources: the scene tree, the
// selection and the asset catalogue. stdout carries only MCP messages; logs go to
// stderr.
#include "f3d/core/system.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kServerName = "forge3d";
constexpr const char* kServerVersion = "0.1.0";
constexpr const char* kLatestProtocol = "2025-06-18";

void log(const std::string& message) {
    std::cerr << "forge3d-mcp: " << message << std::endl;
}

// --- The editor's token file and process ------------------------------------------------------

/// Where the editor writes {port, token} by default (SDL's per-user pref path + rpc.json).
fs::path default_token_file() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) {
        return fs::path(appdata) / "forge3d" / "forge3d" / "rpc.json";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / "Library" / "Application Support" / "forge3d" / "forge3d" / "rpc.json";
    }
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) {
        return fs::path(data) / "forge3d" / "forge3d" / "rpc.json";
    }
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".local" / "share" / "forge3d" / "forge3d" / "rpc.json";
    }
#endif
    return {};
}

std::optional<json> read_token_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    json info = json::parse(file, nullptr, false);
    if (info.is_discarded() || !info.contains("port") || !info.contains("token")) {
        return std::nullopt;
    }
    return info;
}

/// A child process we started (the headless editor).
class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() { stop(); }

    bool start(const fs::path& program, const std::vector<std::string>& args) {
#if defined(_WIN32)
        std::wstring command = L"\"" + program.wstring() + L"\"";
        for (const std::string& arg : args) {
            command += L" \"" + fs::path(arg).wstring() + L"\"";
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // No inherited handles: our stdout is the MCP channel, the editor's output must not reach it.
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &info)) {
            return false;
        }
        CloseHandle(info.hThread);
        process_ = info.hProcess;
        return true;
#else
        std::vector<std::string> storage = {program.string()};
        storage.insert(storage.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (std::string& s : storage) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO); // keep our stdout clean
        const int rc = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        return rc == 0;
#endif
    }

    [[nodiscard]] bool running() const {
#if defined(_WIN32)
        return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
        return pid_ > 0 && waitpid(pid_, nullptr, WNOHANG) == 0;
#endif
    }

    void stop() {
#if defined(_WIN32)
        if (process_) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 10000);
            CloseHandle(process_);
            process_ = nullptr;
        }
#else
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
#endif
    }

private:
#if defined(_WIN32)
    HANDLE process_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

// --- The editor's JSON-RPC server -------------------------------------------------------------

class RpcClient {
public:
    ~RpcClient() { socket_.stop(); }

    bool connect(int port, const std::string& token, std::string& error) {
        socket_.setUrl("ws://127.0.0.1:" + std::to_string(port));
        socket_.disableAutomaticReconnection();
        socket_.disablePerMessageDeflate();
        socket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
            std::lock_guard lock(mutex_);
            if (message->type == ix::WebSocketMessageType::Open) {
                open_ = true;
            } else if (message->type == ix::WebSocketMessageType::Close ||
                       message->type == ix::WebSocketMessageType::Error) {
                closed_ = true;
            } else if (message->type == ix::WebSocketMessageType::Message) {
                try {
                    json reply = json::parse(message->str, nullptr, false);
                    if (!reply.is_discarded() && reply.contains("id") && reply["id"].is_number_integer()) {
                        // The key first: assigning std::move(reply) evaluates (and empties) it first.
                        const long long key = reply["id"].get<long long>();
                        replies_[key] = std::move(reply);
                    }
                } catch (const std::exception& e) {
                    log(std::string("bad reply from the editor: ") + e.what());
                }
                // Notifications (events) are ignored: MCP clients poll resources.
            }
            ready_.notify_all();
        });
        socket_.start();
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait_for(lock, std::chrono::seconds(10), [this] { return open_ || closed_; }) || !open_) {
                error = "cannot connect to ws://127.0.0.1:" + std::to_string(port);
                return false;
            }
        }
        const json auth = call("session.authenticate", {{"token", token}});
        if (!auth.contains("result")) {
            error = "authentication failed: " + auth.dump();
            return false;
        }
        return true;
    }

    /// The JSON-RPC reply ({result} or {error}).
    json call(const std::string& method, const json& params, std::chrono::seconds timeout = std::chrono::seconds(300)) {
        const long long id = next_id_++;
        socket_.sendText(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, timeout, [&] { return replies_.contains(id) || closed_; }) ||
            !replies_.contains(id)) {
            return {{"error",
                     {{"code", -32000},
                      {"message", closed_ ? "the editor closed the connection" : "the editor didn't answer in time"}}}};
        }
        json reply = std::move(replies_[id]);
        replies_.erase(id);
        return reply;
    }

    [[nodiscard]] bool closed() {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    ix::WebSocket socket_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::map<long long, json> replies_;
    std::atomic<long long> next_id_{1};
    bool open_ = false;
    bool closed_ = false;
};

// --- MCP --------------------------------------------------------------------------------------

std::string tool_name(const std::string& op_id) {
    std::string name = op_id;
    for (char& c : name) {
        if (c == '.') {
            c = '_';
        }
    }
    return name;
}

json text_content(const std::string& text) {
    return json::array({{{"type", "text"}, {"text", text}}});
}

json tool_error(const std::string& message) {
    return {{"content", text_content(message)}, {"isError", true}};
}

json object_schema(json properties) {
    return {{"type", "object"}, {"properties", std::move(properties)}};
}

class McpServer {
public:
    explicit McpServer(RpcClient& rpc) : rpc_(rpc) {}

    /// The reply to one MCP message, or nullopt for notifications.
    std::optional<json> handle(const json& message) {
        if (!message.is_object() || !message.contains("method")) {
            if (message.is_object() && message.contains("id")) {
                return error(message["id"], -32600, "invalid request");
            }
            return std::nullopt; // a response to something we never sent: ignore
        }
        const std::string method = message["method"].get<std::string>();
        const bool has_id = message.contains("id");
        const json id = has_id ? message["id"] : json(nullptr);
        const json params = message.value("params", json::object());
        if (!has_id) {
            return std::nullopt; // notifications/initialized, notifications/cancelled, ...
        }
        try {
            if (method == "initialize") {
                return result(id, initialize(params));
            }
            if (method == "ping") {
                return result(id, json::object());
            }
            if (method == "tools/list") {
                return result(id, {{"tools", list_tools()}});
            }
            if (method == "tools/call") {
                return result(id, call_tool(params.value("name", ""), params.value("arguments", json::object())));
            }
            if (method == "resources/list") {
                return result(id, {{"resources", list_resources()}});
            }
            if (method == "resources/read") {
                return read_resource(id, params.value("uri", ""));
            }
            return error(id, -32601, "method not found: " + method);
        } catch (const std::exception& e) {
            return error(id, -32603, e.what());
        }
    }

private:
    static json result(const json& id, json value) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(value)}};
    }
    static json error(const json& id, int code, const std::string& message) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    }

    json initialize(const json& params) {
        std::string version = params.value("protocolVersion", kLatestProtocol);
        if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18") {
            version = kLatestProtocol;
        }
        return {{"protocolVersion", version},
                {"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
                {"instructions",
                 "forge3d is a 3D scene editor. Every scene change is an undoable operation: call the operation "
                 "tools (entity_create, entity_set_property, ...), inspect with scene_query / entity_get or the "
                 "forge3d://scene resource, and look at the result with screenshot. Entities are named by GUID "
                 "strings; property paths are \"Component.field\" (e.g. \"Transform.translation\")."}};
    }

    /// Refreshes the tool -> operation map from ops.list.
    json list_tools() {
        const json reply = rpc_.call("ops.list", json::object());
        if (!reply.contains("result")) {
            throw std::runtime_error("ops.list failed: " + reply.dump());
        }
        json tools = json::array();
        op_for_tool_.clear();
        for (const json& op : reply["result"]) {
            const std::string id = op["id"].get<std::string>();
            const std::string name = tool_name(id);
            if (op_for_tool_.contains(name)) {
                continue; // a name collision: the first wins
            }
            op_for_tool_[name] = id;
            tools.push_back({{"name", name},
                             {"title", op.value("name", id)},
                             {"description", op.value("name", id) + " (forge3d operation " + id +
                                                 "; undoable, returns the number of undo steps)"},
                             {"inputSchema", op.value("schema", object_schema(json::object()))}});
        }
        const json guid = {{"type", "string"}, {"format", "uuid"}};
        tools.push_back(
            {{"name", "screenshot"},
             {"title", "Screenshot"},
             {"description", "Renders the scene and returns a PNG image (the editor's view, a camera "
                             "entity or a bookmark)."},
             {"inputSchema",
              object_schema(
                  {{"width", {{"type", "integer"}, {"default", 960}}},
                   {"height", {{"type", "integer"}, {"default", 540}}},
                   {"mode", {{"type", "string"}, {"enum", {"lit", "unlit", "wireframe", "normals", "cascades"}}}},
                   {"camera", guid},
                   {"bookmark", {{"type", "string"}}}})}});
        tools.push_back({{"name", "undo"},
                         {"description", "Undoes the last operation."},
                         {"inputSchema", object_schema(json::object())}});
        tools.push_back({{"name", "redo"},
                         {"description", "Redoes the last undone operation."},
                         {"inputSchema", object_schema(json::object())}});
        tools.push_back(
            {{"name", "scene_query"},
             {"description", "Lists entities (guid, name, parent, components), filtered by a name "
                             "substring and/or a component type."},
             {"inputSchema", object_schema({{"name", {{"type", "string"}}}, {"component", {{"type", "string"}}}})}});
        tools.push_back(
            {{"name", "entity_get"},
             {"description", "Every component of an entity, as JSON."},
             {"inputSchema", {{"type", "object"}, {"properties", {{"entity", guid}}}, {"required", {"entity"}}}}});
        tools.push_back(
            {{"name", "terrain_sample"},
             {"description", "Terrain heights (world metres) under [x, z] world points, plus the terrain's size, "
                             "height range, layer count and hashes of its heights, surface and painted layers. With "
                             "surface: true, the heights of the ground as shaped by roads and rivers."},
             {"inputSchema",
              {{"type", "object"},
               {"properties",
                {{"entity", guid},
                 {"points",
                  {{"type", "array"},
                   {"items", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}}}}},
                 {"surface", {{"type", "boolean"}}}}},
               {"required", {"entity"}}}}});
        tools.push_back(
            {{"name", "asset_search"},
             {"description", "Searches the open project's assets by text, type and tags."},
             {"inputSchema", object_schema({{"text", {{"type", "string"}}},
                                            {"type", {{"type", "string"}, {"enum", {"model", "texture", ""}}}},
                                            {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}})}});
        return tools;
    }

    json call_tool(const std::string& name, const json& arguments) {
        const json args = arguments.is_object() ? arguments : json::object();
        const auto as_text = [](const json& reply) {
            if (!reply.contains("result")) {
                return tool_error(reply.value("error", json::object()).value("message", std::string("failed")));
            }
            return json{{"content", text_content(reply["result"].dump(2))},
                        {"structuredContent", wrap(reply["result"])}};
        };
        if (name == "screenshot") {
            json params = args;
            params.erase("path"); // always returned inline
            params["width"] = args.value("width", 960);
            params["height"] = args.value("height", 540);
            const json reply = rpc_.call("viewport.capture", params);
            if (!reply.contains("result")) {
                return tool_error(reply.value("error", json::object()).value("message", std::string("capture failed")));
            }
            return {{"content",
                     json::array(
                         {{{"type", "image"}, {"data", reply["result"]["png_base64"]}, {"mimeType", "image/png"}}})}};
        }
        if (name == "undo") {
            return as_text(rpc_.call("ops.undo", json::object()));
        }
        if (name == "redo") {
            return as_text(rpc_.call("ops.redo", json::object()));
        }
        if (name == "scene_query") {
            return as_text(rpc_.call("scene.query", args));
        }
        if (name == "entity_get") {
            return as_text(rpc_.call("entity.get", args));
        }
        if (name == "terrain_sample") {
            json result = rpc_.call("terrain.info", {{"entity", args.value("entity", "")}});
            if (result.contains("result") && args.contains("points")) {
                const json sampled = rpc_.call("terrain.sample", args);
                if (!sampled.contains("result")) {
                    return as_text(sampled);
                }
                result["result"]["heights"] = sampled["result"]["heights"];
            }
            return as_text(result);
        }
        if (name == "asset_search") {
            return as_text(rpc_.call("asset.search", args));
        }
        if (op_for_tool_.empty()) {
            list_tools(); // a client may call without listing first
        }
        const auto it = op_for_tool_.find(name);
        if (it == op_for_tool_.end()) {
            return tool_error("unknown tool '" + name + "'");
        }
        return as_text(rpc_.call("ops.run", {{"id", it->second}, {"params", args}}));
    }

    /// structuredContent must be an object.
    static json wrap(const json& value) { return value.is_object() ? value : json{{"result", value}}; }

    static json list_resources() {
        return json::array({{{"uri", "forge3d://scene"},
                             {"name", "scene"},
                             {"title", "Scene tree"},
                             {"description", "Every entity: guid, name, parent and component types, in scene order."},
                             {"mimeType", "application/json"}},
                            {{"uri", "forge3d://selection"},
                             {"name", "selection"},
                             {"title", "Selection"},
                             {"description", "The selected entities and the active one."},
                             {"mimeType", "application/json"}},
                            {{"uri", "forge3d://assets"},
                             {"name", "assets"},
                             {"title", "Asset catalogue"},
                             {"description", "The open project's assets (guid, path, name, type)."},
                             {"mimeType", "application/json"}}});
    }

    json read_resource(const json& id, const std::string& uri) {
        json reply;
        if (uri == "forge3d://scene") {
            reply = rpc_.call("scene.query", json::object());
        } else if (uri == "forge3d://selection") {
            reply = rpc_.call("selection.get", json::object());
        } else if (uri == "forge3d://assets") {
            reply = rpc_.call("asset.search", json::object());
            if (!reply.contains("result")) {
                reply = {{"result", json::array()}}; // no project open: an empty catalogue
            }
        } else {
            return error(id, -32002, "unknown resource " + uri);
        }
        if (!reply.contains("result")) {
            return error(id, -32603, reply.value("error", json::object()).value("message", std::string("failed")));
        }
        return result(
            id, {{"contents",
                  json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", reply["result"].dump(2)}}})}});
    }

    RpcClient& rpc_;
    std::map<std::string, std::string> op_for_tool_;
};

// --- main -------------------------------------------------------------------------------------

constexpr const char* kUsage = R"(Usage: forge3d-mcp [options]

An MCP server (stdio) for forge3d. Connects to a running editor's JSON-RPC server
(start the editor with --rpc, or enable Preferences > Automation), or starts a
headless editor when none is running.

Options:
  --token-file <path>   the editor's {port, token} file (default: the user config directory)
  --editor <path>       forge3d-editor to start when needed (default: next to forge3d-mcp)
  --spawn-editor        always start a headless editor of our own
  --help                this help
)";

int run(int argc, char** argv) {
    fs::path token_file = default_token_file();
    fs::path editor = f3d::core::executable_dir() / "forge3d-editor";
#if defined(_WIN32)
    editor += ".exe";
#endif
    bool spawn = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--token-file" && i + 1 < argc) {
            token_file = argv[++i];
        } else if (arg == "--editor" && i + 1 < argc) {
            editor = argv[++i];
        } else if (arg == "--spawn-editor") {
            spawn = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cerr << kUsage;
            return 0;
        } else {
            log("unknown argument " + arg);
            std::cerr << kUsage;
            return 2;
        }
    }

    ix::initNetSystem();
    RpcClient rpc;
    Child child;
    std::string error;
    bool connected = false;
    if (!spawn) {
        if (const auto info = read_token_file(token_file)) {
            connected = rpc.connect((*info)["port"].get<int>(), (*info)["token"].get<std::string>(), error);
            if (!connected) {
                log("the editor in " + token_file.string() + " isn't reachable (" + error + "): starting one");
            }
        }
    }
    if (!connected) {
        // A headless editor of our own, with its own token file.
        const fs::path own =
            fs::temp_directory_path() /
            ("forge3d-mcp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        if (!child.start(editor, {"--hidden", "--rpc=0", "--rpc-token-file", own.string()})) {
            log("cannot start " + editor.string());
            return 1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        std::optional<json> info;
        while (!(info = read_token_file(own))) {
            if (!child.running() || std::chrono::steady_clock::now() > deadline) {
                log("the headless editor didn't start its RPC server");
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!rpc.connect((*info)["port"].get<int>(), (*info)["token"].get<std::string>(), error)) {
            log(error);
            return 1;
        }
        log("started a headless editor");
    }

    McpServer server(rpc);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") {
            continue;
        }
        const json message = json::parse(line, nullptr, false);
        std::optional<json> reply;
        if (message.is_discarded()) {
            reply =
                json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}};
        } else if (message.is_array()) {
            json replies = json::array();
            for (const json& item : message) {
                if (auto r = server.handle(item)) {
                    replies.push_back(std::move(*r));
                }
            }
            if (!replies.empty()) {
                reply = std::move(replies);
            }
        } else {
            reply = server.handle(message);
        }
        if (reply) {
            std::cout << reply->dump() << '\n' << std::flush;
        }
        if (rpc.closed()) {
            log("the editor closed the connection");
            return 1;
        }
    }
    return 0; // stdin closed: the client is done (our editor, if any, stops with `child`)
}

} // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    return run(argc, argv);
}
