// SPDX-License-Identifier: Apache-2.0
#pragma once

// A per-frame render graph: passes declare what they read and write, the graph
// culls passes nobody needs, places transient textures (aliasing those whose
// lifetimes don't overlap), works out the state transitions and runs the rest
// in declaration order. See docs/architecture/render.md.
//
//   graph::RenderGraph g;
//   auto out   = g.import_texture("Display", display, rhi::ResourceState::ShaderResource);
//   auto color = g.create_texture("HDR", {w, h, rhi::Format::RGBA16Float});
//   struct Lit { graph::TextureHandle color; };
//   const Lit& lit = g.add_pass<Lit>("lit",
//       [&](graph::PassBuilder& b, Lit& d) { d.color = b.write(color); },
//       [](const Lit& d, graph::PassContext& ctx) { ctx.cmd.set_render_target(ctx.texture(d.color)); ... });
//   ...
//   g.compile();
//   g.execute(device.context(), pool);

#include "f3d/render/rhi/device.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f3d::render::graph {

/// How a pass uses a texture.
enum class Access : std::uint8_t {
    ShaderRead,   ///< sampled / loaded in a shader (read)
    RenderTarget, ///< colour attachment (write)
    DepthWrite,   ///< depth attachment, written (write)
    DepthTest,    ///< depth attachment, tested but not written (read)
    Storage,      ///< unordered access (write)
    CopySource,   ///< (read)
    CopyDest,     ///< (write)
};

[[nodiscard]] bool is_write(Access access) noexcept;
/// The resource state a texture must be in for `access`.
[[nodiscard]] rhi::ResourceState required_state(Access access) noexcept;
[[nodiscard]] std::string_view to_string(Access access) noexcept;

/// Size and format of a graph texture. Bind flags follow from how passes use it.
struct TextureDesc {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    rhi::Format format = rhi::Format::RGBA8Unorm;
    std::uint32_t mip_levels = 1;

    friend bool operator==(const TextureDesc&, const TextureDesc&) = default;
};

/// A version of a graph texture. Every write returns a new version; passes after
/// it must use the new handle.
struct TextureHandle {
    static constexpr std::uint32_t kInvalid = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t resource = kInvalid;
    std::uint32_t version = 0;

    [[nodiscard]] bool valid() const noexcept { return resource != kInvalid; }
    friend bool operator==(const TextureHandle&, const TextureHandle&) = default;
};

class RenderGraph;

/// Declares one pass's resource accesses (inside RenderGraph::add_pass).
class PassBuilder {
public:
    /// Reads `texture` (a read-only access).
    TextureHandle read(TextureHandle texture, Access access = Access::ShaderRead);
    /// Writes `texture` and returns its new version. The pass also depends on the
    /// previous contents (draws on top) when an earlier pass wrote them.
    [[nodiscard]] TextureHandle write(TextureHandle texture, Access access = Access::RenderTarget);
    /// The pass has effects outside the graph (readback, timers): never culled.
    void side_effect() noexcept;

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, std::uint32_t pass) noexcept : graph_(graph), pass_(pass) {}
    RenderGraph& graph_;
    std::uint32_t pass_;
};

/// What a pass sees while it runs.
class PassContext {
public:
    rhi::CommandContext& cmd;
    /// The physical texture behind a handle.
    [[nodiscard]] const rhi::Texture& texture(TextureHandle handle) const;

private:
    friend class RenderGraph;
    PassContext(rhi::CommandContext& c, const RenderGraph& graph) noexcept : cmd(c), graph_(graph) {}
    const RenderGraph& graph_;
};

/// Creates and keeps the GPU textures behind transient resources, across frames.
/// A texture unused for a few frames is released.
class TransientPool {
public:
    explicit TransientPool(rhi::Device& device) : device_(&device) {}
    /// A texture matching (desc, bind) not yet handed out this frame.
    [[nodiscard]] rhi::Texture acquire(const TextureDesc& desc, rhi::BindFlags bind, std::string_view name);
    /// Ends the frame: textures not used for `keep_frames` frames are released.
    void end_frame(std::uint32_t keep_frames = 3);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); }

private:
    struct Entry {
        TextureDesc desc;
        rhi::BindFlags bind = rhi::BindFlags::None;
        rhi::Texture texture;
        std::uint64_t last_frame = 0;
        bool in_use = false;
    };
    rhi::Device* device_;
    std::vector<Entry> entries_;
    std::uint64_t frame_ = 1;
};

/// A state change the graph emits before a pass (or at the end of the frame).
struct Barrier {
    std::uint32_t resource = 0;
    rhi::ResourceState before = rhi::ResourceState::Undefined; ///< as far as the graph knows
    rhi::ResourceState after = rhi::ResourceState::Undefined;
};

class RenderGraph {
public:
    using Execute = std::function<void(PassContext&)>;

    RenderGraph() = default;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    /// An external texture (never aliased). Its last version counts as a graph output.
    /// `final_state`: the state it is left in after the frame (Undefined: whatever the last pass used).
    TextureHandle import_texture(std::string name, rhi::Texture texture, const TextureDesc& desc,
                                 rhi::ResourceState final_state = rhi::ResourceState::Undefined);
    /// As above, size and format from the texture.
    TextureHandle import_texture(std::string name, rhi::Texture texture,
                                 rhi::ResourceState final_state = rhi::ResourceState::Undefined);
    /// A texture that lives only inside this frame.
    TextureHandle create_texture(std::string name, const TextureDesc& desc);
    /// Keeps the passes that produce `texture` (e.g. a transient read back after execute()).
    void mark_output(TextureHandle texture);

    /// Adds a pass. `setup(builder)` runs now and declares the accesses.
    void add_pass(std::string name, const std::function<void(PassBuilder&)>& setup, Execute execute);
    /// Adds a pass with typed data filled by `setup(builder, data)` and read by `execute(data, ctx)`.
    template <class Data, class Setup, class Exec>
    const Data& add_pass(std::string name, Setup&& setup, Exec&& execute) {
        auto data = std::make_shared<Data>();
        add_pass(
            std::move(name), [&](PassBuilder& b) { setup(b, *data); },
            [data, exec = std::forward<Exec>(execute)](PassContext& ctx) { exec(std::as_const(*data), ctx); });
        return *data;
    }

    /// Culls, computes lifetimes, aliasing and barriers. Needs no GPU.
    void compile();
    /// Runs the surviving passes (compiles first if needed). Each pass gets a GPU timer
    /// and a profiler zone named after it when `timers` is set.
    void execute(rhi::CommandContext& cmd, TransientPool& pool, bool timers = true);

    // --- Inspection (after compile) -----------------------------------------------

    [[nodiscard]] std::size_t pass_count() const noexcept { return passes_.size(); }
    [[nodiscard]] const std::string& pass_name(std::uint32_t pass) const { return passes_[pass].name; }
    [[nodiscard]] bool is_culled(std::uint32_t pass) const { return passes_[pass].culled; }
    /// Surviving passes in execution order.
    [[nodiscard]] const std::vector<std::uint32_t>& execution_order() const noexcept { return order_; }
    [[nodiscard]] std::size_t resource_count() const noexcept { return resources_.size(); }
    [[nodiscard]] const std::string& resource_name(std::uint32_t resource) const { return resources_[resource].name; }
    [[nodiscard]] bool is_imported(std::uint32_t resource) const { return resources_[resource].imported; }
    /// The physical texture slot of a transient resource; -1 if imported or unused.
    [[nodiscard]] int physical_slot(std::uint32_t resource) const { return resources_[resource].slot; }
    [[nodiscard]] std::size_t physical_slot_count() const noexcept { return slots_.size(); }
    /// First and last position in execution_order() that use the resource ({-1, -1} if unused).
    [[nodiscard]] std::pair<int, int> lifetime(std::uint32_t resource) const {
        return {resources_[resource].first, resources_[resource].last};
    }
    [[nodiscard]] rhi::BindFlags bind_flags(std::uint32_t resource) const { return resources_[resource].bind; }
    /// Barriers emitted before the pass (by pass index).
    [[nodiscard]] const std::vector<Barrier>& barriers_before(std::uint32_t pass) const {
        return passes_[pass].barriers;
    }
    [[nodiscard]] const std::vector<Barrier>& final_barriers() const noexcept { return final_barriers_; }

    /// Graphviz (dot) description: passes (culled ones grey), resources with their
    /// physical slot, read and write edges.
    [[nodiscard]] std::string to_graphviz() const;

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Use {
        std::uint32_t resource;
        std::uint32_t version; ///< read: the version read; write: the version produced
        Access access;
    };
    struct Pass {
        std::string name;
        Execute execute;
        std::vector<Use> reads; ///< includes the implicit read of a write's previous version
        std::vector<Use> writes;
        std::vector<Use> uses; ///< declared accesses, in order (for states)
        bool side_effect = false;
        bool culled = false;
        std::vector<Barrier> barriers;
    };
    struct Resource {
        std::string name;
        TextureDesc desc;
        bool imported = false;
        bool output = false;  ///< latest version is kept (imported or marked)
        rhi::Texture texture; ///< imported, or acquired at execute
        rhi::ResourceState final_state = rhi::ResourceState::Undefined;
        std::vector<std::int64_t> producers; ///< per version: the pass that wrote it (-1: none)
        rhi::BindFlags bind = rhi::BindFlags::None;
        int first = -1;
        int last = -1;
        int slot = -1;
    };
    struct Slot {
        TextureDesc desc;
        rhi::BindFlags bind = rhi::BindFlags::None;
        int last = -1;
    };

    TextureHandle add_resource(std::string name, const TextureDesc& desc, bool imported, rhi::Texture texture,
                               rhi::ResourceState final_state);
    void check(TextureHandle handle, std::string_view what) const;
    void cull();
    void compute_lifetimes();
    void alias();
    void compute_barriers();

    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> order_;
    std::vector<Barrier> final_barriers_;
    bool compiled_ = false;
};

} // namespace f3d::render::graph
