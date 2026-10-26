// SPDX-License-Identifier: Apache-2.0
#include "f3d/render/graph/render_graph.hpp"

#include "f3d/core/assert.hpp"
#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace f3d::render::graph {

bool is_write(Access access) noexcept {
    switch (access) {
    case Access::RenderTarget:
    case Access::DepthWrite:
    case Access::Storage:
    case Access::CopyDest:
        return true;
    case Access::ShaderRead:
    case Access::DepthTest:
    case Access::CopySource:
        break;
    }
    return false;
}

rhi::ResourceState required_state(Access access) noexcept {
    switch (access) {
    case Access::ShaderRead:
        return rhi::ResourceState::ShaderResource;
    case Access::RenderTarget:
        return rhi::ResourceState::RenderTarget;
    case Access::DepthWrite:
    case Access::DepthTest: // bound as the (writable) depth target, just not written
        return rhi::ResourceState::DepthWrite;
    case Access::Storage:
        return rhi::ResourceState::UnorderedAccess;
    case Access::CopySource:
        return rhi::ResourceState::CopySource;
    case Access::CopyDest:
        return rhi::ResourceState::CopyDest;
    }
    return rhi::ResourceState::Undefined;
}

std::string_view to_string(Access access) noexcept {
    switch (access) {
    case Access::ShaderRead:
        return "read";
    case Access::RenderTarget:
        return "render target";
    case Access::DepthWrite:
        return "depth write";
    case Access::DepthTest:
        return "depth test";
    case Access::Storage:
        return "storage";
    case Access::CopySource:
        return "copy source";
    case Access::CopyDest:
        return "copy dest";
    }
    return "?";
}

namespace {

rhi::BindFlags bind_for(Access access) noexcept {
    switch (access) {
    case Access::ShaderRead:
        return rhi::BindFlags::ShaderResource;
    case Access::RenderTarget:
        return rhi::BindFlags::RenderTarget;
    case Access::DepthWrite:
    case Access::DepthTest:
        return rhi::BindFlags::DepthStencil;
    case Access::Storage:
        return rhi::BindFlags::UnorderedAccess;
    case Access::CopySource:
    case Access::CopyDest:
        break;
    }
    return rhi::BindFlags::None;
}

} // namespace

// --- PassBuilder / PassContext ----------------------------------------------------------

TextureHandle PassBuilder::read(TextureHandle texture, Access access) {
    F3D_ASSERT(!is_write(access), "PassBuilder::read with a write access");
    graph_.check(texture, "read");
    auto& pass = graph_.passes_[pass_];
    pass.reads.push_back({texture.resource, texture.version, access});
    pass.uses.push_back({texture.resource, texture.version, access});
    return texture;
}

TextureHandle PassBuilder::write(TextureHandle texture, Access access) {
    F3D_ASSERT(is_write(access), "PassBuilder::write with a read access");
    graph_.check(texture, "write");
    auto& resource = graph_.resources_[texture.resource];
    auto& pass = graph_.passes_[pass_];
    // Drawing on top of earlier contents depends on whoever produced them.
    if (resource.producers[texture.version] >= 0 || resource.imported) {
        pass.reads.push_back({texture.resource, texture.version, access});
    }
    const auto version = static_cast<std::uint32_t>(resource.producers.size());
    resource.producers.push_back(pass_);
    pass.writes.push_back({texture.resource, version, access});
    pass.uses.push_back({texture.resource, version, access});
    return {texture.resource, version};
}

void PassBuilder::side_effect() noexcept {
    graph_.passes_[pass_].side_effect = true;
}

const rhi::Texture& PassContext::texture(TextureHandle handle) const {
    F3D_ASSERT(handle.valid() && handle.resource < graph_.resources_.size(), "invalid texture handle");
    return graph_.resources_[handle.resource].texture;
}

// --- TransientPool ----------------------------------------------------------------------

rhi::Texture TransientPool::acquire(const TextureDesc& desc, rhi::BindFlags bind, std::string_view name) {
    for (Entry& e : entries_) {
        if (!e.in_use && e.desc == desc && e.bind == bind) {
            e.in_use = true;
            e.last_frame = frame_;
            return e.texture;
        }
    }
    rhi::TextureDesc td;
    td.name = std::string(name);
    td.width = desc.width;
    td.height = desc.height;
    td.format = desc.format;
    td.bind = bind;
    td.mip_levels = desc.mip_levels;
    Entry entry{desc, bind, device_->create_texture(td), frame_, true};
    if (!entry.texture) {
        return {};
    }
    entries_.push_back(entry);
    return entry.texture;
}

void TransientPool::end_frame(std::uint32_t keep_frames) {
    for (Entry& e : entries_) {
        e.in_use = false;
    }
    std::erase_if(entries_, [&](const Entry& e) { return e.last_frame + keep_frames < frame_; });
    ++frame_;
}

// --- RenderGraph: declaration ---------------------------------------------------------

TextureHandle RenderGraph::add_resource(std::string name, const TextureDesc& desc, bool imported, rhi::Texture texture,
                                        rhi::ResourceState final_state) {
    compiled_ = false;
    Resource r;
    r.name = std::move(name);
    r.desc = desc;
    r.imported = imported;
    r.output = imported;
    r.texture = std::move(texture);
    r.final_state = final_state;
    r.producers.push_back(-1); // version 0: initial contents
    resources_.push_back(std::move(r));
    return {static_cast<std::uint32_t>(resources_.size() - 1), 0};
}

TextureHandle RenderGraph::import_texture(std::string name, rhi::Texture texture, const TextureDesc& desc,
                                          rhi::ResourceState final_state) {
    return add_resource(std::move(name), desc, true, std::move(texture), final_state);
}

TextureHandle RenderGraph::import_texture(std::string name, rhi::Texture texture, rhi::ResourceState final_state) {
    TextureDesc desc;
    if (texture) {
        desc = {texture.width(), texture.height(), texture.format(), 1};
    }
    return add_resource(std::move(name), desc, true, std::move(texture), final_state);
}

TextureHandle RenderGraph::create_texture(std::string name, const TextureDesc& desc) {
    return add_resource(std::move(name), desc, false, {}, rhi::ResourceState::Undefined);
}

void RenderGraph::mark_output(TextureHandle texture) {
    check(texture, "mark_output");
    resources_[texture.resource].output = true;
    compiled_ = false;
}

void RenderGraph::check(TextureHandle handle, std::string_view what) const {
    F3D_VERIFY(handle.valid() && handle.resource < resources_.size(), "render graph: invalid handle");
    const Resource& r = resources_[handle.resource];
    F3D_VERIFY(handle.version < r.producers.size(), "render graph: invalid version");
    if (handle.version + 1 != r.producers.size()) {
        F3D_LOG_WARN("render graph: {} of '{}' uses version {} but version {} exists", what, r.name, handle.version,
                     r.producers.size() - 1);
    }
}

void RenderGraph::add_pass(std::string name, const std::function<void(PassBuilder&)>& setup, Execute execute) {
    compiled_ = false;
    Pass pass;
    pass.name = std::move(name);
    pass.execute = std::move(execute);
    passes_.push_back(std::move(pass));
    PassBuilder builder(*this, static_cast<std::uint32_t>(passes_.size() - 1));
    if (setup) {
        setup(builder);
    }
}

// --- RenderGraph: compile ---------------------------------------------------------------

void RenderGraph::cull() {
    // Reference counts: a pass is needed by the versions it produces (plus side
    // effects); a version is needed by its readers (plus being an output).
    std::vector<std::uint32_t> pass_refs(passes_.size(), 0);
    std::vector<std::vector<std::uint32_t>> version_refs(resources_.size());
    for (std::size_t r = 0; r < resources_.size(); ++r) {
        version_refs[r].assign(resources_[r].producers.size(), 0);
        if (resources_[r].output) {
            ++version_refs[r].back();
        }
    }
    for (std::size_t p = 0; p < passes_.size(); ++p) {
        passes_[p].culled = false;
        pass_refs[p] = static_cast<std::uint32_t>(passes_[p].writes.size()) + (passes_[p].side_effect ? 1u : 0u);
        for (const Use& use : passes_[p].reads) {
            ++version_refs[use.resource][use.version];
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> unused; // (resource, version)
    for (std::uint32_t r = 0; r < resources_.size(); ++r) {
        for (std::uint32_t v = 0; v < version_refs[r].size(); ++v) {
            if (version_refs[r][v] == 0) {
                unused.emplace_back(r, v);
            }
        }
    }
    while (!unused.empty()) {
        const auto [r, v] = unused.back();
        unused.pop_back();
        const std::int64_t producer = resources_[r].producers[v];
        if (producer < 0) {
            continue;
        }
        auto& refs = pass_refs[static_cast<std::size_t>(producer)];
        if (refs == 0 || --refs != 0) {
            continue;
        }
        Pass& pass = passes_[static_cast<std::size_t>(producer)];
        pass.culled = true;
        for (const Use& use : pass.reads) {
            if (--version_refs[use.resource][use.version] == 0) {
                unused.emplace_back(use.resource, use.version);
            }
        }
    }
    order_.clear();
    for (std::uint32_t p = 0; p < passes_.size(); ++p) {
        if (!passes_[p].culled) {
            order_.push_back(p);
        }
    }
}

void RenderGraph::compute_lifetimes() {
    for (Resource& r : resources_) {
        r.first = r.last = -1;
        r.bind = rhi::BindFlags::None;
        r.slot = -1;
    }
    for (std::size_t i = 0; i < order_.size(); ++i) {
        const int position = static_cast<int>(i);
        for (const Use& use : passes_[order_[i]].uses) {
            Resource& r = resources_[use.resource];
            if (r.first < 0) {
                r.first = position;
            }
            r.last = position;
            r.bind = r.bind | bind_for(use.access);
        }
    }
}

void RenderGraph::alias() {
    slots_.clear();
    std::vector<std::uint32_t> transient;
    for (std::uint32_t r = 0; r < resources_.size(); ++r) {
        if (!resources_[r].imported && resources_[r].first >= 0) {
            transient.push_back(r);
        }
    }
    std::stable_sort(transient.begin(), transient.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return resources_[a].first < resources_[b].first; });
    for (const std::uint32_t r : transient) {
        Resource& res = resources_[r];
        // Reuse a compatible slot whose last user finished before this resource starts.
        int chosen = -1;
        for (std::size_t s = 0; s < slots_.size(); ++s) {
            if (slots_[s].desc == res.desc && slots_[s].bind == res.bind && slots_[s].last < res.first) {
                chosen = static_cast<int>(s);
                break;
            }
        }
        if (chosen < 0) {
            slots_.push_back({res.desc, res.bind, -1});
            chosen = static_cast<int>(slots_.size() - 1);
        }
        slots_[static_cast<std::size_t>(chosen)].last = res.last;
        res.slot = chosen;
    }
}

void RenderGraph::compute_barriers() {
    // State per physical texture: imported resources by index, slots after them.
    const auto key = [&](std::uint32_t r) {
        return resources_[r].imported ? r : static_cast<std::uint32_t>(resources_.size() + resources_[r].slot);
    };
    std::map<std::uint32_t, rhi::ResourceState> state;
    for (Pass& pass : passes_) {
        pass.barriers.clear();
    }
    for (const std::uint32_t p : order_) {
        Pass& pass = passes_[p];
        for (const Use& use : pass.uses) {
            const rhi::ResourceState wanted = required_state(use.access);
            auto [it, inserted] = state.try_emplace(key(use.resource), rhi::ResourceState::Undefined);
            const bool already_listed = std::any_of(pass.barriers.begin(), pass.barriers.end(),
                                                    [&](const Barrier& b) { return b.resource == use.resource; });
            if (already_listed) {
                F3D_VERIFY(it->second == wanted, "render graph: a pass uses a texture in two states");
                continue;
            }
            if (it->second != wanted || wanted == rhi::ResourceState::UnorderedAccess) {
                pass.barriers.push_back({use.resource, it->second, wanted});
                it->second = wanted;
            }
        }
    }
    final_barriers_.clear();
    for (std::uint32_t r = 0; r < resources_.size(); ++r) {
        const Resource& res = resources_[r];
        if (!res.imported || res.final_state == rhi::ResourceState::Undefined) {
            continue;
        }
        const auto it = state.find(key(r));
        const rhi::ResourceState current = it == state.end() ? rhi::ResourceState::Undefined : it->second;
        if (current != res.final_state) {
            final_barriers_.push_back({r, current, res.final_state});
        }
    }
}

void RenderGraph::compile() {
    F3D_ZONE;
    cull();
    compute_lifetimes();
    alias();
    compute_barriers();
    compiled_ = true;
}

// --- RenderGraph: execute ---------------------------------------------------------------

void RenderGraph::execute(rhi::CommandContext& cmd, TransientPool& pool, bool timers) {
    F3D_ZONE;
    if (!compiled_) {
        compile();
    }
    std::vector<rhi::Texture> physical(slots_.size());
    for (std::size_t s = 0; s < slots_.size(); ++s) {
        std::string name = "Transient";
        for (const Resource& r : resources_) {
            if (r.slot == static_cast<int>(s)) {
                name = r.name;
                break;
            }
        }
        physical[s] = pool.acquire(slots_[s].desc, slots_[s].bind, name);
    }
    for (Resource& r : resources_) {
        if (!r.imported) {
            r.texture = r.slot >= 0 ? physical[static_cast<std::size_t>(r.slot)] : rhi::Texture{};
        }
    }

    std::vector<rhi::TextureTransition> transitions;
    const auto emit = [&](const std::vector<Barrier>& barriers) {
        transitions.clear();
        for (const Barrier& b : barriers) {
            if (resources_[b.resource].texture) {
                transitions.push_back({resources_[b.resource].texture, b.after});
            }
        }
        if (!transitions.empty()) {
            cmd.transition(transitions);
        }
    };
    PassContext context(cmd, *this);
    for (const std::uint32_t p : order_) {
        Pass& pass = passes_[p];
        F3D_ZONE_N("Render graph pass");
        F3D_ZONE_TEXT(pass.name.c_str(), pass.name.size());
        emit(pass.barriers);
        if (!pass.execute) {
            continue;
        }
        if (timers) {
            rhi::ScopedGpuTimer timer(cmd, pass.name);
            pass.execute(context);
        } else {
            pass.execute(context);
        }
    }
    emit(final_barriers_);
}

// --- RenderGraph: debug dump ------------------------------------------------------------

namespace {
std::string dot_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}
} // namespace

std::string RenderGraph::to_graphviz() const {
    std::ostringstream dot;
    dot << "digraph RenderGraph {\n  rankdir=LR;\n  node [fontname=\"Helvetica\", fontsize=10];\n";
    for (std::size_t p = 0; p < passes_.size(); ++p) {
        const Pass& pass = passes_[p];
        dot << "  p" << p << " [shape=box, style=\"rounded,filled\", fillcolor=\""
            << (pass.culled ? "#dddddd" : "#ffd28c") << "\", label=\"" << dot_escape(pass.name)
            << (pass.culled ? "\\n(culled)" : "") << (pass.side_effect ? "\\n(side effect)" : "") << "\"];\n";
    }
    for (std::size_t r = 0; r < resources_.size(); ++r) {
        const Resource& res = resources_[r];
        dot << "  r" << r << " [shape=ellipse, style=filled, fillcolor=\"" << (res.imported ? "#9fd3ff" : "#c8f0c8")
            << "\", label=\"" << dot_escape(res.name) << "\\n"
            << res.desc.width << "x" << res.desc.height << " "
            << (res.imported ? "imported" : (res.slot >= 0 ? "slot " + std::to_string(res.slot) : "unused"))
            << "\"];\n";
    }
    for (std::size_t p = 0; p < passes_.size(); ++p) {
        const Pass& pass = passes_[p];
        for (const Use& use : pass.uses) {
            if (is_write(use.access)) {
                dot << "  p" << p << " -> r" << use.resource << " [color=\"#c0392b\", label=\"v" << use.version << " "
                    << to_string(use.access) << "\"];\n";
            } else {
                dot << "  r" << use.resource << " -> p" << p << " [color=\"#2c6fbb\", label=\"v" << use.version << " "
                    << to_string(use.access) << "\"];\n";
            }
        }
    }
    dot << "}\n";
    return dot.str();
}

} // namespace f3d::render::graph
