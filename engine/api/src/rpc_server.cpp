// SPDX-License-Identifier: Apache-2.0
// The WebSocket side of the JSON-RPC server (phases.md P5.2). IXWebSocket runs a
// thread per connection; those threads only queue messages. poll() (main thread)
// runs them through the dispatcher and sends the replies.
#include "f3d/api/rpc.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <fstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace f3d::api {

struct RpcServer::Impl {
    std::unique_ptr<ix::WebSocketServer> server;

    struct Inbound {
        std::string connection;
        std::string message;
    };
    struct Connection {
        ix::WebSocket* socket = nullptr; ///< valid while in the map (erased before IXWebSocket frees it)
        RpcSession session;
    };

    std::mutex mutex; ///< guards inbound and connections (socket threads vs the main thread)
    std::deque<Inbound> inbound;
    std::unordered_map<std::string, Connection> connections;
    std::vector<std::string> events; ///< change events of the call being run, sent after its reply
};

RpcServer::RpcServer(RpcServices services, RpcServerOptions options)
    : options_(std::move(options)), dispatcher_(services, generate_token()), changes_(services.changes),
      impl_(std::make_unique<Impl>()) {}

RpcServer::~RpcServer() {
    stop();
}

bool RpcServer::start(std::string* error) {
    F3D_ZONE;
    const auto set_error = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };
    ix::initNetSystem();
    const int port = options_.port == 0 ? ix::getFreePort() : options_.port;
    if (port <= 0) {
        return set_error("no free port");
    }
    // Loopback only: nothing on the network can reach it.
    impl_->server = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1", ix::SocketServer::kDefaultTcpBacklog, 16);
    impl_->server->disablePerMessageDeflate();
    Impl* impl = impl_.get();
    const std::size_t max_bytes = options_.max_message_bytes;
    impl_->server->setOnClientMessageCallback([impl, max_bytes](std::shared_ptr<ix::ConnectionState> state,
                                                                ix::WebSocket& socket,
                                                                const ix::WebSocketMessagePtr& message) {
        const std::string& id = state->getId();
        std::lock_guard lock(impl->mutex);
        switch (message->type) {
        case ix::WebSocketMessageType::Open:
            impl->connections[id] = {&socket, {}};
            break;
        case ix::WebSocketMessageType::Close:
        case ix::WebSocketMessageType::Error:
            impl->connections.erase(id);
            break;
        case ix::WebSocketMessageType::Message:
            if (message->str.size() > max_bytes) {
                socket.close(1009, "message too big");
                break;
            }
            impl->inbound.push_back({id, message->str});
            break;
        default:
            break;
        }
    });
    const auto [ok, listen_error] = impl_->server->listen();
    if (!ok) {
        impl_->server.reset();
        return set_error("cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + listen_error);
    }
    impl_->server->start();
    port_ = static_cast<std::uint16_t>(port);

    if (changes_) {
        subscription_ = changes_->subscribe([this](const ops::ChangeEvent& event) {
            // Called on the main thread while a call runs: queue it for after the reply.
            std::lock_guard lock(impl_->mutex);
            for (auto& [id, connection] : impl_->connections) {
                if (auto text = dispatcher_.event_for(event, connection.session)) {
                    impl_->events.push_back(id + '\n' + *text);
                }
            }
        });
    }

    if (!options_.token_file.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options_.token_file.parent_path(), ec);
        std::ofstream file(options_.token_file, std::ios::binary | std::ios::trunc);
#if defined(_WIN32)
        const int pid = _getpid();
#else
        const int pid = static_cast<int>(getpid());
#endif
        file << nlohmann::json{{"port", port_}, {"token", token()}, {"pid", pid}}.dump(2) << '\n';
        file.close();
        if (!file) {
            stop();
            return set_error("cannot write " + options_.token_file.string());
        }
        // Only this user may read the token (the default location, the user's own
        // config directory, is already private on Windows).
        std::filesystem::permissions(options_.token_file,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
    }
    F3D_LOG_INFO("JSON-RPC server on ws://127.0.0.1:{} (token in {})", port_,
                 options_.token_file.empty() ? std::string("memory") : options_.token_file.string());
    return true;
}

void RpcServer::stop() {
    if (changes_ && subscription_) {
        changes_->unsubscribe(subscription_);
        subscription_ = 0;
    }
    if (impl_ && impl_->server) {
        impl_->server->stop();
        impl_->server.reset();
        std::lock_guard lock(impl_->mutex);
        impl_->connections.clear();
        impl_->inbound.clear();
    }
    if (!options_.token_file.empty() && port_ != 0) {
        std::error_code ec;
        std::filesystem::remove(options_.token_file, ec);
    }
    port_ = 0;
}

void RpcServer::poll() {
    if (!impl_->server) {
        return;
    }
    F3D_ZONE;
    for (;;) {
        Impl::Inbound item;
        RpcSession session;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->inbound.empty()) {
                break;
            }
            item = std::move(impl_->inbound.front());
            impl_->inbound.pop_front();
            const auto it = impl_->connections.find(item.connection);
            if (it == impl_->connections.end()) {
                continue; // closed meanwhile
            }
            session = it->second.session;
        }
        // The call runs without the lock: it may take a while (captures) and emit events.
        const std::optional<std::string> reply = dispatcher_.handle(item.message, session);
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->connections.find(item.connection);
        if (it == impl_->connections.end()) {
            flush_events();
            continue;
        }
        it->second.session = session;
        if (reply) {
            it->second.socket->sendText(*reply);
        }
        if (session.closing) {
            it->second.socket->close(4001, "not authenticated");
        }
        flush_events();
    }
    // Changes made outside a call (the editor's UI) are sent too.
    std::lock_guard lock(impl_->mutex);
    flush_events();
}

void RpcServer::flush_events() {
    for (const std::string& event : impl_->events) {
        const auto newline = event.find('\n');
        const auto target = impl_->connections.find(event.substr(0, newline));
        if (target != impl_->connections.end()) {
            target->second.socket->sendText(event.substr(newline + 1));
        }
    }
    impl_->events.clear();
}

std::size_t RpcServer::connections() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->connections.size();
}

} // namespace f3d::api
