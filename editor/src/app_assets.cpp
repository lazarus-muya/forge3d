// SPDX-License-Identifier: Apache-2.0
// The App's project assets (phases.md P3.1-P3.3): the asset index, cooked
// models for MeshRenderers, placing assets in the scene, tags, thumbnails.
#include "app.hpp"

#include "f3d/assets/pipeline.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"
#include "f3d/ops/prefab.hpp"
#include "f3d/ops/script.hpp"
#include "f3d/scene/components.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace f3d::editor {
namespace {

nlohmann::json slots_json(const std::vector<core::AssetRef>& slots) {
    nlohmann::json out = nlohmann::json::array();
    for (const core::AssetRef& r : slots) {
        out.push_back({{"guid", r.guid.is_null() ? std::string() : r.guid.to_string()}, {"path", r.path}});
    }
    return out;
}

} // namespace

std::string App::unused_project_path(const std::string& dir, const std::string& name,
                                     std::string_view extension) const {
    std::string base;
    for (const char c : name) {
        base.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
    }
    if (base.empty()) {
        base = "untitled";
    }
    const std::string stem = "project://" + dir + "/" + base;
    std::string path = stem + std::string(extension);
    for (int n = 2; vfs_.exists(path); ++n) {
        path = stem + "_" + std::to_string(n) + std::string(extension);
    }
    return path;
}

std::shared_ptr<const assets::CookedModel> App::cooked_model(const core::Guid& guid) {
    if (const auto it = cooked_models_.find(guid); it != cooked_models_.end()) {
        return it->second;
    }
    std::string error;
    auto model = assets::load_cooked_model(vfs_, guid, &error);
    if (!model) {
        F3D_LOG_DEBUG("Cooked model {}: {}", guid.to_string(), error);
        return nullptr;
    }
    auto shared = std::make_shared<const assets::CookedModel>(std::move(*model));
    cooked_models_[guid] = shared;
    return shared;
}

std::shared_ptr<const assets::CookedMesh> App::cooked_mesh(const core::AssetRef& ref) {
    const auto model = cooked_model(ref.guid);
    const auto hash = ref.path.rfind("#mesh/");
    if (!model || hash == std::string::npos) {
        return nullptr;
    }
    std::size_t index = 0;
    const char* begin = ref.path.data() + hash + 6;
    const char* end = ref.path.data() + ref.path.size();
    if (std::from_chars(begin, end, index).ec != std::errc() || index >= model->meshes.size()) {
        return nullptr;
    }
    return {model, &model->meshes[index]}; // aliasing: keeps the model alive
}

std::shared_ptr<const assets::MeshData> App::resolve_mesh(const core::AssetRef& ref) {
    const auto mesh = cooked_mesh(ref);
    return mesh ? std::make_shared<const assets::MeshData>(mesh->to_mesh_data(0)) : nullptr;
}

std::shared_ptr<const assets::Material> App::resolve_material(const core::AssetRef& ref) {
    const std::string_view sub = assets::sub_asset(ref);
    if (sub.starts_with("material/") && !ref.guid.is_null()) {
        // A cooked model's material.
        const auto model = cooked_model(ref.guid);
        std::size_t index = 0;
        const char* begin = sub.data() + 9;
        const char* end = sub.data() + sub.size();
        if (!model || std::from_chars(begin, end, index).ec != std::errc() || index >= model->materials.size()) {
            return nullptr;
        }
        return std::make_shared<const assets::Material>(
            assets::rebase_textures(model->materials[index], core::AssetRef{ref.guid, ref.path}));
    }
    // A project material file: by its current path when the GUID is known (files move).
    std::string path = ref.path;
    if (!ref.guid.is_null()) {
        if (const assets::AssetRecord* record = asset_db_.find(ref.guid)) {
            path = record->path;
        }
    }
    const auto text = vfs_.read_text(path);
    std::string error = "cannot read";
    auto material = text ? assets::read_material(*text, &error) : std::nullopt;
    if (!material) {
        F3D_LOG_DEBUG("Material {}: {}", path, error);
        return nullptr;
    }
    return std::make_shared<const assets::Material>(std::move(*material));
}

void App::make_editable_material(const core::Guid& entity, std::size_t slot) {
    const entt::entity e = world_.find(entity);
    const auto* renderer = e == entt::null ? nullptr : world_.registry().try_get<scene::MeshRenderer>(e);
    const auto mesh = renderer ? meshes_.get(renderer->mesh) : nullptr;
    if (!mesh) {
        return;
    }
    if (!vfs_.mounted("project")) {
        F3D_LOG_WARN("Make Editable Copy: save the scene first (materials are stored next to it)");
        return;
    }
    const std::vector<assets::SubMesh> submeshes = assets::submeshes_of(*mesh);
    if (slot >= submeshes.size()) {
        return;
    }
    const core::AssetRef source = assets::MeshLibrary::slot_material(*renderer, slot, submeshes[slot]);
    const auto material = source.empty() ? nullptr : meshes_.material(source);
    assets::Material copy = material ? *material : assets::Material{};
    if (copy.name.empty()) {
        copy.name = "Material";
    }
    const std::string path = unused_project_path("materials", copy.name, assets::kMaterialExtension);
    std::vector<core::AssetRef> slots = renderer->materials;
    slots.resize(std::max(slots.size(), slot + 1));
    slots[slot] = {core::Guid{}, path};
    op_stack_.begin_transaction("Make material editable");
    if (scene_editor_.run("material.create", {{"path", path}, {"material", assets::material_to_json(copy)}}) &&
        scene_editor_.run("entity.set_property", {{"entity", entity.to_string()},
                                                  {"path", "MeshRenderer.materials"},
                                                  {"value", slots_json(slots)}})) {
        op_stack_.end_transaction();
        F3D_LOG_INFO("Created {}", path);
        if (!is_automated_run()) {
            start_asset_scan();
        }
    } else {
        op_stack_.abort_transaction();
    }
}

void App::apply_material(const core::Guid& material, const glm::vec3& origin, const glm::vec3& direction) {
    const assets::AssetRecord* record = asset_db_.find(material);
    if (!record) {
        return;
    }
    snap_bvh_.build(world_, meshes_);
    const assets::SceneHit hit = snap_bvh_.raycast(origin, direction);
    const auto* renderer = hit.hit ? world_.registry().try_get<scene::MeshRenderer>(hit.entity) : nullptr;
    const auto mesh = renderer ? meshes_.get(renderer->mesh) : nullptr;
    if (!mesh) {
        F3D_LOG_WARN("Drop {} on a mesh to apply it", record->name());
        return;
    }
    const std::vector<core::AssetRef> slots(assets::submeshes_of(*mesh).size(), {material, record->path});
    const core::Guid entity = world_.guid(hit.entity);
    scene_editor_.run("entity.set_property", {{"entity", entity.to_string()},
                                              {"path", "MeshRenderer.materials"},
                                              {"value", slots_json(slots)}});
    scene_editor_.set_selection({entity}, entity);
}

std::string App::create_material(const core::AssetRef& copy_from) {
    if (!vfs_.mounted("project")) {
        F3D_LOG_WARN("New Material: save the scene first (materials are stored next to it)");
        return {};
    }
    assets::Material material;
    material.name = "Material";
    if (!copy_from.empty()) {
        const auto source = meshes_.material(copy_from);
        if (!source) {
            F3D_LOG_WARN("Duplicate Material: {} could not be read", copy_from.path);
            return {};
        }
        material = *source;
        material.name = (material.name.empty() ? std::string("Material") : material.name) + " Copy";
    }
    const std::string path = unused_project_path("materials", material.name, assets::kMaterialExtension);
    if (!scene_editor_.run("material.create", {{"path", path}, {"material", assets::material_to_json(material)}})) {
        return {};
    }
    F3D_LOG_INFO("Created {}", path);
    if (!is_automated_run()) {
        start_asset_scan(); // lists it in the library and the browser
    }
    return path;
}

void App::delete_material(const core::Guid& material) {
    const assets::AssetRecord* record = asset_db_.find(material);
    if (!record || record->type() != "material") {
        return;
    }
    const std::string path = record->path;
    const std::string name = record->name();
    const auto uses = [&](const core::AssetRef& ref) {
        return (!ref.guid.is_null() && ref.guid == material) || ref.path == path;
    };
    // One undo step: the slots that use it, then the file.
    std::vector<std::pair<core::Guid, std::vector<core::AssetRef>>> users;
    for (const auto [e, renderer] : world_.registry().view<scene::MeshRenderer>().each()) {
        if (std::any_of(renderer.materials.begin(), renderer.materials.end(), uses)) {
            std::vector<core::AssetRef> slots = renderer.materials;
            std::replace_if(slots.begin(), slots.end(), uses, core::AssetRef{});
            users.emplace_back(world_.guid(e), std::move(slots));
        }
    }
    op_stack_.begin_transaction("Delete material " + name);
    bool ok = true;
    for (const auto& [entity, slots] : users) {
        ok = ok && scene_editor_.run("entity.set_property", {{"entity", entity.to_string()},
                                                             {"path", "MeshRenderer.materials"},
                                                             {"value", slots_json(slots)}});
    }
    if (!ok || !scene_editor_.run("material.delete", {{"path", path}})) {
        op_stack_.abort_transaction();
        F3D_LOG_ERROR("Could not delete {}", path);
        return;
    }
    op_stack_.end_transaction();
    F3D_LOG_INFO("Deleted {} ({} objects used it)", path, users.size());
    if (!is_automated_run()) {
        start_asset_scan();
    }
}

void App::assign_material_to_selection(const core::Guid& material) {
    const assets::AssetRecord* record = asset_db_.find(material);
    if (!record || record->type() != "material") {
        return;
    }
    const core::AssetRef ref{material, record->path};
    std::vector<std::pair<core::Guid, std::size_t>> targets; // entity, slot count
    for (const core::Guid& guid : world_.selection().entities()) {
        const entt::entity e = world_.find(guid);
        const auto* renderer = e == entt::null ? nullptr : world_.registry().try_get<scene::MeshRenderer>(e);
        if (const auto mesh = renderer ? meshes_.get(renderer->mesh) : nullptr) {
            targets.emplace_back(guid, std::max<std::size_t>(assets::submeshes_of(*mesh).size(), 1));
        }
    }
    if (targets.empty()) {
        F3D_LOG_WARN("Assign {}: select objects with a mesh first", record->name());
        return;
    }
    op_stack_.begin_transaction("Assign " + record->name());
    bool ok = true;
    for (const auto& [guid, count] : targets) {
        ok = ok && scene_editor_.run("entity.set_property",
                                     {{"entity", guid.to_string()},
                                      {"path", "MeshRenderer.materials"},
                                      {"value", slots_json(std::vector<core::AssetRef>(count, ref))}});
    }
    if (ok) {
        op_stack_.end_transaction();
    } else {
        op_stack_.abort_transaction();
    }
}

void App::assign_material_slot(const core::Guid& entity, std::size_t slot, const core::Guid& material) {
    const assets::AssetRecord* record = asset_db_.find(material);
    const entt::entity e = world_.find(entity);
    const auto* renderer = e == entt::null ? nullptr : world_.registry().try_get<scene::MeshRenderer>(e);
    if (!record || !renderer) {
        return;
    }
    if (record->type() != "material") {
        F3D_LOG_WARN("{} is not a material: drop materials on a slot", record->path);
        return;
    }
    std::vector<core::AssetRef> slots = renderer->materials;
    slots.resize(std::max(slots.size(), slot + 1));
    slots[slot] = {material, record->path};
    scene_editor_.run("entity.set_property", {{"entity", entity.to_string()},
                                              {"path", "MeshRenderer.materials"},
                                              {"value", slots_json(slots)}});
}

void App::open_asset_index(const std::filesystem::path& project_dir) {
    asset_index_.reset();
    asset_index_ptr_ = nullptr;
    thumbnails_->set_index(nullptr);
    if (project_dir.empty()) {
        return;
    }
    std::string error;
    asset_index_ = assets::AssetIndex::open(project_dir / ".forge3d" / "index.sqlite", &error);
    if (!asset_index_) {
        F3D_LOG_WARN("Asset index unavailable ({}); the browser filters in memory", error);
        return;
    }
    asset_index_ptr_ = asset_index_.get();
    thumbnails_->set_index(asset_index_ptr_);
    // Last session's records: the scan only re-hashes files that changed since.
    asset_db_.seed(asset_index_->load_records());
}

void App::scan_assets_now() {
    F3D_ZONE;
    if (!vfs_.mounted("project")) {
        return;
    }
    asset_db_.rescan(vfs_, "project://");
    std::size_t imported = 0;
    for (const assets::AssetRecord& record : asset_db_.records()) {
        if (record.stale()) {
            const assets::ImportOutcome outcome = assets::import_asset(vfs_, record);
            imported += outcome.ok ? 1 : 0;
            if (!outcome.ok) {
                F3D_LOG_ERROR("Import failed: {}", outcome.error);
            }
        }
    }
    asset_db_.rescan(vfs_, "project://");
    if (asset_index_) {
        asset_index_->sync(asset_db_);
    }
    for (const assets::AssetRecord& record : asset_db_.records()) {
        (void)thumbnails_->get(record);
    }
    thumbnails_->finish_all();
    F3D_LOG_INFO("Assets: {} found, {} imported, {} thumbnails", asset_db_.size(), imported, thumbnails_->ready());
}

void App::place_asset(const core::Guid& guid, const glm::vec3& position) {
    const assets::AssetRecord* record = asset_db_.find(guid);
    if (!record) {
        return;
    }
    if (record->type() == "prefab") {
        const core::Guid root = core::Guid::generate();
        op_stack_.begin_transaction("Place " + record->name());
        if (scene_editor_.run("prefab.instantiate", {{"prefab", record->path},
                                                     {"position", {position.x, position.y, position.z}},
                                                     {"guid", root.to_string()}})) {
            scene_editor_.set_selection({root}, root);
            op_stack_.end_transaction();
            scene_bounds_.add(assets::world_bounds(world_, meshes_, world_.find(root)));
        } else {
            op_stack_.abort_transaction();
        }
        return;
    }
    if (record->type() != "model") {
        F3D_LOG_WARN("{} is a texture: drop models or prefabs into the viewport", record->path);
        return;
    }
    const auto model = record->stale() ? nullptr : cooked_model(guid);
    if (!model) {
        F3D_LOG_WARN("{} isn't imported yet; importing it now, place it again when it's done", record->path);
        import_queue_.enqueue(*record, true);
        return;
    }
    core::Guid root;
    const std::string source = record->path;
    const nlohmann::json script = assets::model_instance_ops(
        record->name(), model->nodes,
        [&](std::size_t mesh) { return core::AssetRef{guid, source + "#mesh/" + std::to_string(mesh)}; }, {}, position,
        root);
    op_stack_.begin_transaction("Place " + record->name());
    const ops::ScriptResult result = ops::run_script(op_stack_, op_registry_, script);
    if (!result.ok) {
        op_stack_.abort_transaction();
        F3D_LOG_ERROR("Could not place {}: {}", record->path, result.error);
        return;
    }
    scene_editor_.set_selection({root}, root);
    op_stack_.end_transaction();
    scene_bounds_.add(assets::world_bounds(world_, meshes_, world_.find(root)));
    F3D_LOG_INFO("Placed {} at ({:.2f}, {:.2f}, {:.2f})", record->name(), position.x, position.y, position.z);
}

void App::create_prefab_from_selection() {
    const core::Guid active = world_.selection().active();
    const entt::entity e = active.is_null() ? entt::null : world_.find(active);
    if (e == entt::null) {
        F3D_LOG_WARN("Create Prefab: select the entity to turn into a prefab");
        return;
    }
    if (!vfs_.mounted("project")) {
        F3D_LOG_WARN("Create Prefab: save the scene first (prefabs are stored next to it)");
        return;
    }
    const std::string path = unused_project_path("prefabs", world_.name(e), ".f3dprefab");
    if (scene_editor_.run("prefab.create_from_selection", {{"entity", active.to_string()}, {"path", path}})) {
        F3D_LOG_INFO("Created prefab {}", path);
        if (!is_automated_run()) {
            start_asset_scan();
        }
    }
}

void App::set_asset_tags(const core::Guid& guid, const std::vector<std::string>& tags) {
    const assets::AssetRecord* record = asset_db_.find(guid);
    if (!record) {
        return;
    }
    const std::string meta_path = record->path + ".meta";
    const auto text = vfs_.read_text(meta_path);
    std::optional<assets::AssetMeta> meta = text ? assets::parse_meta(*text) : std::nullopt;
    if (!meta) {
        F3D_LOG_ERROR("Cannot update tags: {} is missing or invalid", meta_path);
        return;
    }
    meta->tags = tags;
    if (!vfs_.write_text(meta_path, assets::meta_to_json(*meta))) {
        F3D_LOG_ERROR("Cannot write {}", meta_path);
        return;
    }
    start_asset_scan(); // picks up the new tags (and syncs the index)
}

void App::apply_texture(const core::Guid& texture, const glm::vec3& origin, const glm::vec3& direction) {
    const assets::AssetRecord* record = asset_db_.find(texture);
    if (!record) {
        return;
    }
    snap_bvh_.build(world_, meshes_);
    const assets::SceneHit hit = snap_bvh_.raycast(origin, direction);
    if (!hit.hit || !world_.registry().all_of<scene::MeshRenderer>(hit.entity)) {
        F3D_LOG_WARN("Drop {} on a mesh to apply it", record->name());
        return;
    }
    if (record->stale()) {
        import_queue_.enqueue(*record, true); // applied now, drawn once cooked
    }
    const core::Guid entity = world_.guid(hit.entity);
    scene_editor_.run("entity.set_property", {{"entity", entity.to_string()},
                                              {"path", "MeshRenderer.texture"},
                                              {"value", {{"guid", texture.to_string()}, {"path", record->path}}}});
    scene_editor_.set_selection({entity}, entity);
}

void App::watch_asset_sources(double dt_seconds) {
    // Automated runs are deterministic: they never react to files changing underneath.
    if (is_automated_run() || !vfs_.mounted("project")) {
        return;
    }
    source_watcher_.sync(vfs_, asset_db_);
    watch_poll_timer_ += dt_seconds;
    if (watch_poll_in_flight_ || watch_poll_timer_ < 1.0) {
        return;
    }
    watch_poll_timer_ = 0.0;
    watch_poll_in_flight_ = true;
    jobs_.submit(
        core::jobs::Priority::Background, watch_token_,
        [this](const core::jobs::CancellationToken&) { return source_watcher_.poll(); },
        [this](std::vector<core::Guid> changed) {
            watch_poll_in_flight_ = false;
            if (changed.empty()) {
                return;
            }
            for (const core::Guid& guid : changed) {
                if (const assets::AssetRecord* record = asset_db_.find(guid)) {
                    F3D_LOG_INFO("Asset source changed: {}", record->path);
                }
            }
            start_asset_scan(); // re-hashes the changed sources; stale ones are re-imported
        });
}

void App::asset_imported(const core::Guid& guid) {
    // The cooked files changed: drop the cached copies so scenes and thumbnails use the new ones.
    cooked_models_.erase(guid);
    meshes_.invalidate(guid);
    meshes_.invalidate_materials(); // material files (and copies of model materials) may have changed
    if (residency_) {
        residency_->invalidate(guid); // hot swap: reloaded on the next request
    }
    // An edited prefab: its instances in the scene follow (undoable, keeps overrides).
    if (const assets::AssetRecord* record = asset_db_.find(guid); record && record->type() == "prefab") {
        if (!ops::prefab::instances_of(world_, record->path).empty()) {
            scene_editor_.run("prefab.update_instances", {{"prefab", record->path}});
        }
    }
}

} // namespace f3d::editor
