// SPDX-License-Identifier: Apache-2.0
// JSON-RPC 2.0 methods (phases.md P5.2). See f3d/api/rpc.hpp.
#include "f3d/api/rpc.hpp"

#include "f3d/core/guid.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain.hpp"
#include "f3d/world/spline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>

namespace f3d::api {
namespace {

/// A JSON-RPC error raised by a method.
struct RpcFailure : std::runtime_error {
    RpcFailure(int code_, const std::string& message) : std::runtime_error(message), code(code_) {}
    int code;
};

[[noreturn]] void fail(int code, const std::string& message) {
    throw RpcFailure(code, message);
}

const nlohmann::json& object_params(const nlohmann::json& params) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (params.is_null()) {
        return empty;
    }
    if (!params.is_object()) {
        fail(kInvalidParams, "params must be an object");
    }
    return params;
}

std::string required_string(const nlohmann::json& params, const char* key) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        fail(kInvalidParams, std::string("'") + key + "' must be a string");
    }
    return it->get<std::string>();
}

entt::entity required_entity(scene::World& world, const nlohmann::json& params) {
    const auto guid = core::Guid::parse(required_string(params, "entity"));
    if (!guid) {
        fail(kInvalidParams, "'entity' must be a GUID");
    }
    const entt::entity e = world.find(*guid);
    if (e == entt::null) {
        fail(kInvalidParams, "no entity " + guid->to_string());
    }
    return e;
}

/// Constant-time comparison (the token).
bool same_secret(const std::string& a, const std::string& b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string base64(const std::vector<unsigned char>& data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t{data[i]} << 16) |
                                (i + 1 < data.size() ? std::uint32_t{data[i + 1]} << 8 : 0u) |
                                (i + 2 < data.size() ? std::uint32_t{data[i + 2]} : 0u);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < data.size() ? kTable[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kTable[n & 63] : '=';
    }
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

nlohmann::json entity_summary(scene::World& world, entt::entity e) {
    nlohmann::json out;
    out["guid"] = world.guid(e).to_string();
    const auto* name = world.registry().try_get<scene::Name>(e);
    out["name"] = name ? name->value : "";
    const entt::entity parent = world.parent(e);
    out["parent"] = parent == entt::null ? nlohmann::json(nullptr) : nlohmann::json(world.guid(parent).to_string());
    nlohmann::json components = nlohmann::json::array();
    scene::for_each_component(world.registry(), e, [&](entt::meta_any any) {
        if (const char* type = any.type().name()) {
            components.push_back(type);
        }
    });
    out["components"] = std::move(components);
    return out;
}

nlohmann::json run_operation(Host& host, const std::string& id, const nlohmann::json& params) {
    std::unique_ptr<ops::Operation> op;
    try {
        op = host.registry().create(id, params);
    } catch (const std::exception& e) {
        fail(kInvalidParams, id + ": bad parameters: " + e.what());
    }
    if (!op) {
        fail(kInvalidParams, "unknown operation '" + id + "'");
    }
    if (!host.stack().execute(std::move(op))) {
        fail(kOperationFailed, id + " failed (invalid parameters for the current scene)");
    }
    return {{"undo_steps", host.stack().undo_count()}};
}

/// FNV-1a 64 of a heightfield's painted layer weights ("" before any were painted).
std::string splat_hash(const terrain::Heightfield& heightfield) {
    const terrain::Splatmap* splat = heightfield.splat();
    if (!splat) {
        return {};
    }
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint32_t m = 0; m < splat->maps(); ++m) {
        const std::uint8_t* bytes = splat->map(m);
        for (std::size_t i = 0; i < splat->map_bytes(); ++i) {
            h = (h ^ bytes[i]) * 0x100000001b3ull;
        }
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h));
    return text;
}

} // namespace

std::string_view to_string(ops::ChangeKind kind) noexcept {
    switch (kind) {
    case ops::ChangeKind::EntityCreated:
        return "entity_created";
    case ops::ChangeKind::EntityDestroyed:
        return "entity_destroyed";
    case ops::ChangeKind::ComponentChanged:
        return "component_changed";
    case ops::ChangeKind::HierarchyChanged:
        return "hierarchy_changed";
    case ops::ChangeKind::SelectionChanged:
        return "selection_changed";
    case ops::ChangeKind::LayersChanged:
        return "layers_changed";
    case ops::ChangeKind::BookmarksChanged:
        return "bookmarks_changed";
    case ops::ChangeKind::ViewRequested:
        return "view_requested";
    case ops::ChangeKind::AssetChanged:
        return "asset_changed";
    }
    return "unknown";
}

nlohmann::json json_schema(const nlohmann::json& rows) {
    nlohmann::json properties = nlohmann::json::object();
    for (const auto& row : rows) {
        const std::string type = row.value("type", "");
        nlohmann::json p;
        if (type == "bool") {
            p["type"] = "boolean";
        } else if (type == "int32" || type == "int64" || type == "int") {
            p["type"] = "integer";
        } else if (type == "uint32") {
            p["type"] = "integer";
            p["minimum"] = 0;
        } else if (type == "float" || type == "double") {
            p["type"] = "number";
        } else if (type == "string") {
            p["type"] = "string";
        } else if (type == "vec2" || type == "vec3" || type == "vec4" || type == "quat") {
            const int n = type == "vec2" ? 2 : (type == "vec3" ? 3 : 4);
            p = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", n}, {"maxItems", n}};
            if (type == "quat") {
                p["description"] = "rotation [w, x, y, z]";
            }
        } else if (type == "guid") {
            p = {{"type", "string"}, {"format", "uuid"}};
        } else if (type == "guid[]") {
            p = {{"type", "array"}, {"items", {{"type", "string"}, {"format", "uuid"}}}};
        } else if (type == "asset_ref") {
            p = {{"type", "object"}, {"properties", {{"guid", {{"type", "string"}}}, {"path", {{"type", "string"}}}}}};
        } else if (type == "asset_ref[]") {
            p = {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties", {{"guid", {{"type", "string"}}}, {"path", {{"type", "string"}}}}}}}};
        } else if (type != "json" && !type.empty()) {
            // Reflected enums are stored as integers.
            p["type"] = "integer";
            p["description"] = type;
        }
        if (row.contains("tooltip")) {
            p["description"] = row["tooltip"];
        }
        if (row.contains("min") && p.value("type", "") != "array") {
            p["minimum"] = row["min"];
            p["maximum"] = row["max"];
        }
        if (row.contains("default")) {
            p["default"] = row["default"];
        }
        if (row.contains("name")) {
            properties[row["name"].get<std::string>()] = std::move(p);
        }
    }
    return {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
}

std::string generate_token() {
    std::random_device device; // the OS's cryptographic source on the supported platforms
    std::array<std::uint32_t, 8> words{};
    for (auto& w : words) {
        w = device();
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const std::uint32_t w : words) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out += kHex[(w >> shift) & 0xF];
        }
    }
    return out;
}

RpcDispatcher::RpcDispatcher(RpcServices services, std::string token)
    : services_(std::move(services)), token_(std::move(token)) {}

std::optional<std::string> RpcDispatcher::handle(const std::string& message, RpcSession& session) {
    nlohmann::json request = nlohmann::json::parse(message, nullptr, false);
    if (request.is_discarded()) {
        session.closing = !session.authenticated; // garbage before authenticating: hang up
        return nlohmann::json{
            {"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", kParseError}, {"message", "parse error"}}}}
            .dump();
    }
    if (request.is_array()) {
        if (request.empty()) {
            return nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", kInvalidRequest}, {"message", "empty batch"}}}}
                .dump();
        }
        nlohmann::json responses = nlohmann::json::array();
        for (const auto& item : request) {
            bool respond = false;
            nlohmann::json response = handle_one(item, session, respond);
            if (respond) {
                responses.push_back(std::move(response));
            }
        }
        if (responses.empty()) {
            return std::nullopt;
        }
        return responses.dump();
    }
    bool respond = false;
    nlohmann::json response = handle_one(request, session, respond);
    if (!respond) {
        return std::nullopt;
    }
    return response.dump();
}

nlohmann::json RpcDispatcher::handle_one(const nlohmann::json& request, RpcSession& session, bool& respond) {
    const auto error = [](const nlohmann::json& id, int code, const std::string& message) {
        return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    };
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method") ||
        !request["method"].is_string()) {
        respond = true;
        session.closing = !session.authenticated;
        return error(request.is_object() && request.contains("id") ? request["id"] : nlohmann::json(nullptr),
                     kInvalidRequest, "invalid request");
    }
    const bool has_id = request.contains("id");
    const nlohmann::json id = has_id ? request["id"] : nlohmann::json(nullptr);
    if (has_id && !id.is_string() && !id.is_number() && !id.is_null()) {
        respond = true;
        return error(nullptr, kInvalidRequest, "invalid id");
    }
    respond = has_id;
    const std::string method = request["method"].get<std::string>();
    if (!session.authenticated && method != "session.authenticate") {
        respond = true; // always tell the client why it's being closed
        session.closing = true;
        return error(id, kNotAuthenticated, "authenticate first (session.authenticate {token})");
    }
    try {
        const nlohmann::json params = request.contains("params") ? request["params"] : nlohmann::json(nullptr);
        nlohmann::json result = call(method, params, session);
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    } catch (const RpcFailure& e) {
        if (e.code == kNotAuthenticated) {
            respond = true;
        }
        return error(id, e.code, e.what());
    } catch (const std::exception& e) {
        return error(id, kInternalError, e.what());
    }
}

nlohmann::json RpcDispatcher::call(const std::string& method, const nlohmann::json& raw_params, RpcSession& session) {
    const nlohmann::json& params = object_params(raw_params);
    if (method == "session.authenticate") {
        const auto it = params.find("token");
        if (it == params.end() || !it->is_string() || !same_secret(it->get<std::string>(), token_)) {
            session.closing = true;
            fail(kNotAuthenticated, "wrong token");
        }
        session.authenticated = true;
        return {{"authenticated", true}, {"api_version", F3D_API_VERSION}};
    }
    if (!services_.host) {
        fail(kUnavailable, "no session");
    }
    Host& host = *services_.host;
    scene::World& world = host.world();

    if (method == "ops.list") {
        nlohmann::json out = nlohmann::json::array();
        for (const ops::OpInfo& info : host.registry().list()) {
            const nlohmann::json rows = ops::param_schema(info);
            out.push_back(
                {{"id", info.id}, {"name", info.display_name}, {"params", rows}, {"schema", json_schema(rows)}});
        }
        return out;
    }
    if (method == "ops.run") {
        const auto it = params.find("params");
        const nlohmann::json op_params = it == params.end() ? nlohmann::json::object() : *it;
        if (!op_params.is_object()) {
            fail(kInvalidParams, "'params' must be an object");
        }
        return run_operation(host, required_string(params, "id"), op_params);
    }
    if (method == "ops.undo") {
        const bool done = host.stack().undo();
        return {{"done", done}, {"undo_steps", host.stack().undo_count()}};
    }
    if (method == "ops.redo") {
        const bool done = host.stack().redo();
        return {{"done", done}, {"undo_steps", host.stack().undo_count()}};
    }
    if (method == "scene.query") {
        const std::string name = lower(params.value("name", ""));
        const std::string component = params.value("component", "");
        nlohmann::json out = nlohmann::json::array();
        for (const entt::entity e : world.depth_first()) {
            nlohmann::json summary = entity_summary(world, e);
            if (!name.empty() && lower(summary["name"].get<std::string>()).find(name) == std::string::npos) {
                continue;
            }
            if (!component.empty()) {
                const auto& list = summary["components"];
                if (std::find(list.begin(), list.end(), component) == list.end()) {
                    continue;
                }
            }
            out.push_back(std::move(summary));
        }
        return out;
    }
    if (method == "selection.get") {
        nlohmann::json entities = nlohmann::json::array();
        for (const core::Guid& guid : world.selection().entities()) {
            if (world.find(guid) != entt::null) {
                entities.push_back(guid.to_string());
            }
        }
        const core::Guid active = world.selection().active();
        return {{"entities", std::move(entities)},
                {"active", active.is_null() ? nlohmann::json(nullptr) : nlohmann::json(active.to_string())}};
    }
    if (method == "entity.get") {
        const entt::entity e = required_entity(world, params);
        nlohmann::json out = entity_summary(world, e);
        nlohmann::json components = nlohmann::json::object();
        scene::for_each_component(world.registry(), e, [&](entt::meta_any any) {
            if (const char* type = any.type().name()) {
                nlohmann::json value;
                core::reflect::value_to_json(value, any);
                components[type] = std::move(value);
            }
        });
        out["components"] = std::move(components);
        nlohmann::json children = nlohmann::json::array();
        for (const entt::entity child : world.children(e)) {
            children.push_back(world.guid(child).to_string());
        }
        out["children"] = std::move(children);
        return out;
    }
    if (method == "terrain.info" || method == "terrain.sample") {
        const entt::entity e = required_entity(world, params);
        const std::shared_ptr<terrain::Heightfield> hf =
            world.registry().all_of<terrain::Terrain>(e) ? terrain::heightfield_of(world, e) : nullptr;
        if (!hf) {
            fail(kInvalidParams, "not a terrain");
        }
        world::update_landscape(world); // roads and riverbeds into the surface
        const std::shared_ptr<terrain::Heightfield> surface = terrain::surface_of(world, e);
        const glm::vec3 origin = terrain::terrain_origin(world, e);
        if (method == "terrain.info") {
            const glm::vec2 range = hf->range();
            const terrain::Terrain& t = world.registry().get<terrain::Terrain>(e);
            return {{"samples", hf->samples()},
                    {"size", hf->size()},
                    {"spacing", hf->spacing()},
                    {"origin", {origin.x, origin.y, origin.z}},
                    {"min_height", range.x},
                    {"max_height", range.y},
                    {"hash", terrain::height_hash(*hf)},
                    {"surface_hash", terrain::height_hash(surface ? *surface : *hf)},
                    {"layers", t.layers.size()},
                    {"splat_hash", splat_hash(*hf)}};
        }
        const terrain::Heightfield& sampled = params.value("surface", false) && surface ? *surface : *hf;
        const auto it = params.find("points");
        if (it == params.end() || !it->is_array()) {
            fail(kInvalidParams, "'points' must be an array of [x, z] (world metres)");
        }
        nlohmann::json heights = nlohmann::json::array();
        for (const auto& p : *it) {
            if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
                fail(kInvalidParams, "each point must be [x, z]");
            }
            const float x = p[0].get<float>() - origin.x;
            const float z = p[1].get<float>() - origin.z;
            heights.push_back(sampled.height_at(x, z) + origin.y);
        }
        return {{"heights", std::move(heights)}};
    }
    if (method == "entity.set") {
        required_entity(world, params);
        if (!params.contains("value")) {
            fail(kInvalidParams, "'value' is required");
        }
        return run_operation(
            host, "entity.set_property",
            {{"entity", params["entity"]}, {"path", required_string(params, "path")}, {"value", params["value"]}});
    }
    if (method == "asset.search") {
        if (!services_.search_assets) {
            fail(kUnavailable, "no asset index in this session (open a project)");
        }
        assets::AssetQuery query;
        query.text = params.value("text", "");
        query.type = params.value("type", "");
        query.limit = params.value("limit", std::size_t{200});
        if (params.contains("tags")) {
            for (const auto& tag : params["tags"]) {
                if (tag.is_string()) {
                    query.tags.push_back(tag.get<std::string>());
                }
            }
        }
        nlohmann::json out = nlohmann::json::array();
        for (const assets::AssetSummary& a : services_.search_assets(query)) {
            out.push_back({{"guid", a.guid.to_string()},
                           {"path", a.path},
                           {"name", a.name},
                           {"type", a.type},
                           {"stale", a.stale}});
        }
        return out;
    }
    if (method == "viewport.capture") {
        if (!services_.capture) {
            fail(kUnavailable, "this session can't render");
        }
        render::raster::CaptureRequest request;
        request.width = params.value("width", 1280u);
        request.height = params.value("height", 720u);
        const auto mode = render::raster::parse_view_mode(params.value("mode", "lit"));
        if (!mode) {
            fail(kInvalidParams, "unknown mode (lit, unlit, wireframe, normals, cascades)");
        }
        request.mode = *mode;
        if (params.contains("camera")) {
            const auto camera = core::Guid::parse(params.value("camera", ""));
            if (!camera) {
                fail(kInvalidParams, "'camera' must be a GUID");
            }
            request.camera = *camera;
        }
        request.bookmark = params.value("bookmark", "");
        const bool to_file = params.contains("path");
        const std::filesystem::path path = to_file ? std::filesystem::path(required_string(params, "path"))
                                                   : std::filesystem::temp_directory_path() /
                                                         ("forge3d-rpc-" + generate_token().substr(0, 16) + ".png");
        request.path = path.string();
        std::string error;
        if (!services_.capture(world, request, &error)) {
            fail(kOperationFailed, "capture failed: " + error);
        }
        nlohmann::json out = {{"width", request.width}, {"height", request.height}};
        if (to_file) {
            out["path"] = path.string();
            return out;
        }
        std::ifstream file(path, std::ios::binary);
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                               std::istreambuf_iterator<char>());
        file.close();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        out["png_base64"] = base64(bytes);
        return out;
    }
    if (method == "events.subscribe") {
        std::set<std::string> kinds;
        if (params.contains("kinds")) {
            if (!params["kinds"].is_array()) {
                fail(kInvalidParams, "'kinds' must be an array of event kinds");
            }
            for (const auto& kind : params["kinds"]) {
                if (kind.is_string()) {
                    kinds.insert(kind.get<std::string>());
                }
            }
        }
        session.subscribed = kinds;
        return {{"subscribed", kinds.empty() ? nlohmann::json("all") : nlohmann::json(kinds)}};
    }
    fail(kMethodNotFound, "unknown method '" + method + "'");
}

std::optional<std::string> RpcDispatcher::event_for(const ops::ChangeEvent& event, const RpcSession& session) const {
    if (!session.authenticated || !session.subscribed) {
        return std::nullopt;
    }
    const std::string kind(to_string(event.kind));
    if (!session.subscribed->empty() && !session.subscribed->contains(kind)) {
        return std::nullopt;
    }
    nlohmann::json params = {{"kind", kind}};
    params["entity"] = event.entity.is_null() ? nlohmann::json(nullptr) : nlohmann::json(event.entity.to_string());
    if (!event.asset.empty()) {
        params["asset"] = event.asset;
    }
    return nlohmann::json{{"jsonrpc", "2.0"}, {"method", "events.changed"}, {"params", std::move(params)}}.dump();
}

} // namespace f3d::api
