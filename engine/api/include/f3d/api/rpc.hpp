// SPDX-License-Identifier: Apache-2.0
#pragma once

// JSON-RPC 2.0 over WebSocket (phases.md P5.2). Off by default; when enabled the
// server listens on 127.0.0.1 only, and a connection must authenticate with the
// session token (written to a user-only file) in its first message.
//
// RpcDispatcher holds the methods (pure logic, testable without sockets); RpcServer
// moves messages between sockets and the dispatcher. Socket threads only queue
// messages: every call runs on the main thread in RpcServer::poll().
//
// Methods (params are JSON objects):
//   session.authenticate {token}                  must come first
//   ops.list {}                                   ids, names, parameter rows and JSON Schemas
//   ops.run {id, params}                          through the history (undoable)
//   ops.undo {} / ops.redo {}
//   scene.query {name?, component?}               entities: guid, name, parent, components
//   selection.get {}                              {entities, active}
//   entity.get {entity}                           every reflected component as JSON
//   entity.set {entity, path, value}              = ops.run entity.set_property
//   terrain.info {entity}                         {samples, size, spacing, origin, min_height, max_height, hash}
//   terrain.sample {entity, points: [[x, z]...]}  {heights} (world metres, bilinear)
//   asset.search {text?, type?, tags?, limit?}
//   viewport.capture {width?, height?, mode?, camera?, bookmark?, path?}
//                                                 path: write the PNG there; else {png_base64}
//   events.subscribe {kinds?}                     then notifications "events.changed"
//                                                 {kind, entity, asset}

#include "f3d/api/host.hpp"
#include "f3d/assets/asset_index.hpp"
#include "f3d/ops/operation.hpp"
#include "f3d/render/raster/capture.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3d::api {

/// JSON-RPC 2.0 error codes (spec) and ours (-32000..-32099: server errors).
enum RpcError : int {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams = -32602,
    kInternalError = -32603,
    kNotAuthenticated = -32001,
    kOperationFailed = -32002,
    kUnavailable = -32003,
};

/// What the dispatcher needs from its host. Optional parts may be empty.
struct RpcServices {
    Host* host = nullptr;
    ops::ChangeBus* changes = nullptr;
    std::function<std::vector<assets::AssetSummary>(const assets::AssetQuery&)> search_assets;
    render::raster::CaptureFunction capture;
};

/// One connection's state.
struct RpcSession {
    bool authenticated = false;
    bool closing = false;                            ///< the server closes it after the reply
    std::optional<std::set<std::string>> subscribed; ///< event kinds (empty set: all)
};

/// A JSON Schema ("type": "object") for an operation's parameter rows (param_schema).
[[nodiscard]] nlohmann::json json_schema(const nlohmann::json& rows);

/// The name of a change kind in events ("entity_created", "component_changed", ...).
[[nodiscard]] std::string_view to_string(ops::ChangeKind kind) noexcept;

class RpcDispatcher {
public:
    RpcDispatcher(RpcServices services, std::string token);

    /// Handles one message (a request, a notification or a batch). Returns the
    /// response text, or nullopt when there is nothing to answer (notifications).
    [[nodiscard]] std::optional<std::string> handle(const std::string& message, RpcSession& session);

    /// The notification for a change event, or nullopt if `session` didn't subscribe to it.
    [[nodiscard]] std::optional<std::string> event_for(const ops::ChangeEvent& event, const RpcSession& session) const;

    [[nodiscard]] const std::string& token() const noexcept { return token_; }

private:
    [[nodiscard]] nlohmann::json handle_one(const nlohmann::json& request, RpcSession& session, bool& respond);
    [[nodiscard]] nlohmann::json call(const std::string& method, const nlohmann::json& params, RpcSession& session);

    RpcServices services_;
    std::string token_;
};

/// A random session token: 32 bytes from the OS RNG, hex.
[[nodiscard]] std::string generate_token();

struct RpcServerOptions {
    std::uint16_t port = 7468;        ///< 0: any free port
    std::filesystem::path token_file; ///< where {port, token} is written (user-only)
    std::size_t max_message_bytes = 16u << 20;
};

/// The WebSocket server. start() on the main thread; poll() every frame.
class RpcServer {
public:
    RpcServer(RpcServices services, RpcServerOptions options);
    ~RpcServer();
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    /// Listens on 127.0.0.1 and writes the token file. False (and `error`) on failure.
    bool start(std::string* error = nullptr);
    void stop();
    /// Runs queued calls and sends replies and events (main thread).
    void poll();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const std::string& token() const noexcept { return dispatcher_.token(); }
    [[nodiscard]] std::size_t connections() const;

private:
    void flush_events(); ///< sends queued change events (the lock held)

    struct Impl;
    RpcServerOptions options_;
    RpcDispatcher dispatcher_;
    ops::ChangeBus* changes_ = nullptr;
    ops::ChangeBus::Token subscription_ = 0;
    std::uint16_t port_ = 0;
    std::unique_ptr<Impl> impl_;
};

} // namespace f3d::api
