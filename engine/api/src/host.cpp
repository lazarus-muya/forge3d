// SPDX-License-Identifier: Apache-2.0
#include "f3d/api/host.hpp"

#include "f3d/core/guid.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/reflect.hpp"
#include "f3d/core/system.hpp"
#include "f3d/ops/operation.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/components.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace f3d::api {
namespace {

thread_local std::string t_error;

F3dResult fail(F3dResult result, std::string message) {
    t_error = std::move(message);
    return result;
}

F3dResult ok() {
    t_error.clear();
    return F3D_OK;
}

core::Guid to_guid(F3dEntity e) noexcept {
    return core::Guid{e.hi, e.lo};
}

F3dEntity to_entity(const core::Guid& g) noexcept {
    return F3dEntity{g.high(), g.low()};
}

scene::World* as_world(F3dWorld* world) noexcept {
    return reinterpret_cast<scene::World*>(world);
}

F3dWorld* as_handle(scene::World& world) noexcept {
    return reinterpret_cast<F3dWorld*>(&world);
}

/// A running C operation's context: where its scene changes are recorded.
struct OpContext {
    ops::Context* context = nullptr;
    const ops::OpRegistry* registry = nullptr;
    std::vector<std::unique_ptr<ops::Operation>>* children = nullptr;
};

OpContext* as_context(F3dOpContext* ctx) noexcept {
    return reinterpret_cast<OpContext*>(ctx);
}

/// Runs an operation as a child of the C operation behind `ctx`.
F3dResult run_child(F3dOpContext* handle, const std::string& id, const nlohmann::json& params) {
    OpContext* ctx = as_context(handle);
    if (!ctx || !ctx->context) {
        return fail(F3D_ERROR_WRONG_CONTEXT, "scene changes need an operation context");
    }
    std::unique_ptr<ops::Operation> op;
    try {
        op = ctx->registry->create(id, params);
    } catch (const std::exception& e) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, id + ": bad parameters: " + e.what());
    }
    if (!op) {
        return fail(F3D_ERROR_NOT_FOUND, "unknown operation '" + id + "'");
    }
    if (!op->execute(*ctx->context)) {
        return fail(F3D_ERROR_FAILED, id + " failed");
    }
    if (op->records_history()) {
        ctx->children->push_back(std::move(op));
    }
    return ok();
}

// --- The function table ----------------------------------------------------------------------

const char* api_last_error() {
    return t_error.c_str();
}

void api_log(F3dLogLevel level, const char* message) {
    const char* text = message ? message : "";
    switch (level) {
    case F3D_LOG_LEVEL_WARN:
        F3D_LOG_WARN("[plugin] {}", text);
        break;
    case F3D_LOG_LEVEL_ERROR:
        F3D_LOG_ERROR("[plugin] {}", text);
        break;
    default:
        F3D_LOG_INFO("[plugin] {}", text);
        break;
    }
}

F3dResult api_register_op(F3dHost* host, const F3dOpDesc* desc) {
    if (!host || !desc) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "register_op: null host or descriptor");
    }
    std::string error;
    if (!register_c_operation(Host::from(host)->registry(), *desc, &error)) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "register_op: " + error);
    }
    return ok();
}

F3dResult api_run_op(F3dHost* handle, const char* id, const char* params_json) {
    if (!handle || !id) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "run_op: null host or id");
    }
    Host& host = *Host::from(handle);
    nlohmann::json params = nlohmann::json::object();
    if (params_json && *params_json) {
        params = nlohmann::json::parse(params_json, nullptr, false);
        if (params.is_discarded()) {
            return fail(F3D_ERROR_INVALID_ARGUMENT, std::string("run_op ") + id + ": invalid JSON parameters");
        }
    }
    std::unique_ptr<ops::Operation> op;
    try {
        op = host.registry().create(id, params);
    } catch (const std::exception& e) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, std::string("run_op ") + id + ": bad parameters: " + e.what());
    }
    if (!op) {
        return fail(F3D_ERROR_NOT_FOUND, std::string("run_op: unknown operation '") + id + "'");
    }
    if (!host.stack().execute(std::move(op))) {
        return fail(F3D_ERROR_FAILED, std::string("run_op ") + id + " failed");
    }
    return ok();
}

F3dResult api_undo(F3dHost* host) {
    if (!host) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "undo: null host");
    }
    return Host::from(host)->stack().undo() ? ok() : fail(F3D_ERROR_FAILED, "nothing to undo");
}

F3dResult api_redo(F3dHost* host) {
    if (!host) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "redo: null host");
    }
    return Host::from(host)->stack().redo() ? ok() : fail(F3D_ERROR_FAILED, "nothing to redo");
}

F3dWorld* api_host_world(F3dHost* host) {
    return host ? as_handle(Host::from(host)->world()) : nullptr;
}

size_t api_entity_count(F3dWorld* world) {
    return world ? as_world(world)->size() : 0;
}

F3dResult api_find_entity(F3dWorld* handle, const char* name, F3dEntity* out) {
    scene::World* world = as_world(handle);
    if (!world || !name || !out) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "find_entity: null argument");
    }
    for (const entt::entity e : world->depth_first()) {
        const auto* n = world->registry().try_get<scene::Name>(e);
        if (n && n->value == name) {
            *out = to_entity(world->guid(e));
            return ok();
        }
    }
    return fail(F3D_ERROR_NOT_FOUND, std::string("no entity named '") + name + "'");
}

F3dResult api_get_property(F3dWorld* handle, F3dEntity entity, const char* path, char* buffer, size_t capacity,
                           size_t* written) {
    scene::World* world = as_world(handle);
    if (!world || !path) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "get_property: null argument");
    }
    const entt::entity e = world->find(to_guid(entity));
    if (e == entt::null) {
        return fail(F3D_ERROR_NOT_FOUND, "get_property: no such entity");
    }
    const std::string_view p(path);
    const auto dot = p.find('.');
    if (dot == std::string_view::npos) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "get_property: expected Component.field");
    }
    entt::meta_any component = scene::component_by_name(world->registry(), e, p.substr(0, dot));
    if (!component) {
        return fail(F3D_ERROR_NOT_FOUND, "get_property: the entity has no " + std::string(p.substr(0, dot)));
    }
    const entt::meta_data field = core::reflect::field_by_name(component.type(), p.substr(dot + 1));
    if (!field) {
        return fail(F3D_ERROR_NOT_FOUND, std::string("get_property: no field ") + path);
    }
    nlohmann::json value;
    core::reflect::value_to_json(value, field.get(component));
    const std::string text = value.dump();
    if (written) {
        *written = text.size() + 1;
    }
    if (!buffer || capacity < text.size() + 1) {
        return fail(F3D_ERROR_BUFFER_TOO_SMALL, "get_property: buffer too small");
    }
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return ok();
}

F3dWorld* api_context_world(F3dOpContext* ctx) {
    OpContext* c = as_context(ctx);
    return c && c->context ? as_handle(c->context->world) : nullptr;
}

F3dResult api_create_entity(F3dOpContext* ctx, const char* name, F3dEntity parent, F3dEntity* out) {
    const core::Guid guid = core::Guid::generate();
    nlohmann::json params = {{"name", name ? name : "Entity"}, {"guid", guid.to_string()}};
    if (parent.hi != 0 || parent.lo != 0) {
        params["parent"] = to_guid(parent).to_string();
    }
    const F3dResult r = run_child(ctx, "entity.create", params);
    if (r == F3D_OK && out) {
        *out = to_entity(guid);
    }
    return r;
}

F3dResult api_delete_entity(F3dOpContext* ctx, F3dEntity entity) {
    return run_child(ctx, "entity.delete", {{"entity", to_guid(entity).to_string()}});
}

F3dResult api_set_property(F3dOpContext* ctx, F3dEntity entity, const char* path, const char* json_value) {
    if (!path || !json_value) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "set_property: null path or value");
    }
    nlohmann::json value = nlohmann::json::parse(json_value, nullptr, false);
    if (value.is_discarded()) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, std::string("set_property ") + path + ": invalid JSON value");
    }
    return run_child(ctx, "entity.set_property",
                     {{"entity", to_guid(entity).to_string()}, {"path", path}, {"value", std::move(value)}});
}

F3dResult api_add_component(F3dOpContext* ctx, F3dEntity entity, const char* component) {
    if (!component) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "add_component: null component");
    }
    return run_child(ctx, "entity.add_component", {{"entity", to_guid(entity).to_string()}, {"component", component}});
}

F3dResult api_run_child_op(F3dOpContext* ctx, const char* id, const char* params_json) {
    if (!id) {
        return fail(F3D_ERROR_INVALID_ARGUMENT, "run_child_op: null id");
    }
    nlohmann::json params = nlohmann::json::object();
    if (params_json && *params_json) {
        params = nlohmann::json::parse(params_json, nullptr, false);
        if (params.is_discarded()) {
            return fail(F3D_ERROR_INVALID_ARGUMENT, std::string("run_child_op ") + id + ": invalid JSON parameters");
        }
    }
    return run_child(ctx, id, params);
}

constexpr F3dApi kApi = {
    F3D_API_VERSION,   sizeof(F3dApi),    api_last_error,    api_log,          api_register_op,   api_run_op,
    api_undo,          api_redo,          api_host_world,    api_entity_count, api_find_entity,   api_get_property,
    api_context_world, api_create_entity, api_delete_entity, api_set_property, api_add_component, api_run_child_op,
};

// --- C operations ----------------------------------------------------------------------------

struct COpDesc {
    std::string id;
    std::string display_name;
    F3dOpExecuteFn execute = nullptr;
    void* user_data = nullptr;
};

class COperation final : public ops::Operation {
public:
    COperation(std::shared_ptr<const COpDesc> desc, const ops::OpRegistry& registry)
        : desc_(std::move(desc)), registry_(registry) {}

    [[nodiscard]] std::string_view id() const override { return desc_->id; }
    [[nodiscard]] std::string display_name() const override { return desc_->display_name; }
    [[nodiscard]] entt::meta_type params_type() const override { return {}; }
    [[nodiscard]] nlohmann::json params_json() const override { return params_; }
    void set_params_json(const nlohmann::json& params) override {
        params_ = params.is_object() ? params : nlohmann::json::object();
    }

    bool execute(ops::Context& context) override {
        if (recorded_) {
            // Redo: replay what the plugin did, without calling it again.
            for (std::size_t i = 0; i < children_.size(); ++i) {
                if (!children_[i]->execute(context)) {
                    for (std::size_t j = i; j-- > 0;) {
                        children_[j]->undo(context);
                    }
                    return false;
                }
            }
            return true;
        }
        OpContext ctx{&context, &registry_, &children_};
        const std::string params = params_.dump();
        const F3dResult result =
            desc_->execute(reinterpret_cast<F3dOpContext*>(&ctx), params.c_str(), desc_->user_data);
        if (result != F3D_OK) {
            F3D_LOG_ERROR("{} failed: {}", desc_->id, t_error.empty() ? "error " + std::to_string(result) : t_error);
            undo(context);
            children_.clear();
            return false;
        }
        recorded_ = true;
        return true;
    }

    void undo(ops::Context& context) override {
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            (*it)->undo(context);
        }
    }

    [[nodiscard]] std::size_t memory_size() const override {
        std::size_t bytes = sizeof(*this);
        for (const auto& child : children_) {
            bytes += child->memory_size();
        }
        return bytes;
    }

private:
    std::shared_ptr<const COpDesc> desc_;
    const ops::OpRegistry& registry_;
    nlohmann::json params_ = nlohmann::json::object();
    std::vector<std::unique_ptr<ops::Operation>> children_;
    bool recorded_ = false;
};

void* open_library(const std::filesystem::path& path, std::string& error) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (!module) {
        error = "could not load (error " + std::to_string(GetLastError()) + ")";
    }
    return module;
#else
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        const char* message = dlerror();
        error = message ? message : "could not load";
    }
    return module;
#endif
}

void* find_symbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void close_library(void* library) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

template <class Fn>
Fn symbol_as(void* library, const char* name) {
    return reinterpret_cast<Fn>(find_symbol(library, name));
}

std::string version_text(std::uint32_t version) {
    return std::to_string(F3D_API_MAJOR(version)) + "." + std::to_string(F3D_API_MINOR(version));
}

} // namespace

bool is_compatible(std::uint32_t plugin_version) noexcept {
    return F3D_API_MAJOR(plugin_version) == F3D_API_VERSION_MAJOR &&
           F3D_API_MINOR(plugin_version) <= F3D_API_VERSION_MINOR;
}

bool register_c_operation(ops::OpRegistry& registry, const F3dOpDesc& desc, std::string* error) {
    const auto set_error = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };
    if (!desc.id || !*desc.id || !desc.execute) {
        return set_error("an operation needs an id and an execute function");
    }
    if (registry.contains(desc.id)) {
        return set_error(std::string("duplicate operation id '") + desc.id + "'");
    }
    nlohmann::json schema = nlohmann::json::array();
    if (desc.params_schema_json && *desc.params_schema_json) {
        schema = nlohmann::json::parse(desc.params_schema_json, nullptr, false);
        if (schema.is_discarded() || !schema.is_array()) {
            return set_error(std::string(desc.id) + ": the parameter schema must be a JSON array");
        }
    }
    auto shared = std::make_shared<COpDesc>();
    shared->id = desc.id;
    shared->display_name = desc.display_name && *desc.display_name ? desc.display_name : desc.id;
    shared->execute = desc.execute;
    shared->user_data = desc.user_data;
    try {
        registry.add({shared->id, shared->display_name, entt::meta_type{}, std::move(schema)},
                     [shared, &registry] { return std::make_unique<COperation>(shared, registry); });
    } catch (const std::exception& e) {
        return set_error(e.what());
    }
    return true;
}

PluginRecord PluginLoader::load(const std::filesystem::path& library) {
    PluginRecord record;
    record.path = library;
    record.name = library.stem().string();
    const auto finish = [&](std::string error) {
        record.error = std::move(error);
        if (record.loaded) {
            F3D_LOG_INFO("Plugin {} {} loaded from {}", record.name, record.version, library.string());
        } else {
            F3D_LOG_ERROR("Plugin {} not loaded: {}", library.string(), record.error);
        }
        plugins_.push_back(record);
        return record;
    };
    std::string error;
    void* module = open_library(library, error);
    if (!module) {
        return finish(error);
    }
    const auto version = symbol_as<F3dPluginApiVersionFn>(module, "f3d_plugin_api_version");
    const auto entry = symbol_as<F3dPluginEntryFn>(module, "f3d_plugin_entry");
    if (!version || !entry) {
        close_library(module);
        return finish("not a forge3d plugin (needs f3d_plugin_api_version and f3d_plugin_entry)");
    }
    const std::uint32_t wanted = version();
    if (!is_compatible(wanted)) {
        close_library(module);
        return finish("built for forge3d API " + version_text(wanted) + ", this host provides " +
                      version_text(F3D_API_VERSION) + " (rebuild the plugin against this forge3d)");
    }
    F3dPluginInfo info{nullptr, nullptr};
    const F3dResult result = entry(&f3d_api_get, host_.handle(), &info);
    if (info.name && *info.name) {
        record.name = info.name;
    }
    if (info.version) {
        record.version = info.version;
    }
    if (result != F3D_OK) {
        // Its code may be referenced by what it registered: keep it loaded.
        return finish("f3d_plugin_entry failed: " + (t_error.empty() ? "error " + std::to_string(result) : t_error));
    }
    record.loaded = true;
    return finish({});
}

std::vector<PluginRecord> PluginLoader::load_directory(const std::filesystem::path& dir) {
    std::vector<PluginRecord> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return out;
    }
#if defined(_WIN32)
    constexpr const char* kExtension = ".dll";
#elif defined(__APPLE__)
    constexpr const char* kExtension = ".dylib";
#else
    constexpr const char* kExtension = ".so";
#endif
    std::vector<std::filesystem::path> libraries;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == kExtension) {
            libraries.push_back(entry.path());
        }
    }
    std::sort(libraries.begin(), libraries.end());
    for (const auto& library : libraries) {
        out.push_back(load(library));
    }
    return out;
}

std::filesystem::path default_plugin_dir() {
    const std::filesystem::path dir = core::executable_dir();
    return dir.empty() ? std::filesystem::path("plugins") : dir / "plugins";
}

} // namespace f3d::api

extern "C" const F3dApi* f3d_api_get(uint32_t requested_version) {
    if (!f3d::api::is_compatible(requested_version)) {
        f3d::api::t_error = "forge3d API " + f3d::api::version_text(requested_version) +
                            " requested, this host provides " + f3d::api::version_text(F3D_API_VERSION);
        return nullptr;
    }
    return &f3d::api::kApi;
}
