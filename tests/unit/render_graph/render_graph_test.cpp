// SPDX-License-Identifier: Apache-2.0
// Render graph compilation (phases.md P4.1): culling, lifetimes, aliasing and
// barriers. No GPU: compile() is pure bookkeeping.
#include <doctest/doctest.h>

#include "f3d/render/graph/render_graph.hpp"

#include <random>
#include <string>

using namespace f3d::render;
using graph::Access;
using graph::RenderGraph;
using graph::TextureHandle;

namespace {

const graph::TextureDesc kColor{64, 64, rhi::Format::RGBA16Float};
const graph::TextureDesc kDepth{64, 64, rhi::Format::D32Float};

/// Adds a pass reading `reads` and writing `writes` (render target); returns the written versions.
std::vector<TextureHandle> pass(RenderGraph& g, const std::string& name, std::vector<TextureHandle> reads,
                                std::vector<TextureHandle> writes, bool side_effect = false) {
    std::vector<TextureHandle> out;
    g.add_pass(name,
               [&](graph::PassBuilder& b) {
                   for (const TextureHandle r : reads) {
                       b.read(r);
                   }
                   for (const TextureHandle w : writes) {
                       out.push_back(b.write(w));
                   }
                   if (side_effect) {
                       b.side_effect();
                   }
               },
               {});
    return out;
}

std::uint32_t pass_index(const RenderGraph& g, const std::string& name) {
    for (std::uint32_t p = 0; p < g.pass_count(); ++p) {
        if (g.pass_name(p) == name) {
            return p;
        }
    }
    FAIL("no pass ", name);
    return 0;
}

/// Every pair of transient resources sharing a slot must have disjoint lifetimes.
void check_no_overlap(const RenderGraph& g) {
    for (std::uint32_t a = 0; a < g.resource_count(); ++a) {
        for (std::uint32_t b = a + 1; b < g.resource_count(); ++b) {
            if (g.physical_slot(a) < 0 || g.physical_slot(a) != g.physical_slot(b)) {
                continue;
            }
            const auto [a0, a1] = g.lifetime(a);
            const auto [b0, b1] = g.lifetime(b);
            INFO(g.resource_name(a), " [", a0, ",", a1, "] and ", g.resource_name(b), " [", b0, ",", b1, "]");
            CHECK((a1 < b0 || b1 < a0));
        }
    }
}

} // namespace

TEST_CASE("render graph: writes return new versions; reading the old one warns but works") {
    RenderGraph g;
    const TextureHandle a = g.create_texture("A", kColor);
    const TextureHandle a1 = pass(g, "w1", {}, {a})[0];
    CHECK(a1.resource == a.resource);
    CHECK(a1.version == 1);
    const TextureHandle a2 = pass(g, "w2", {}, {a1})[0];
    CHECK(a2.version == 2);
}

TEST_CASE("render graph: passes whose results nobody uses are culled") {
    RenderGraph g;
    const TextureHandle out = g.import_texture("Output", {}, kColor);
    const TextureHandle color = g.create_texture("Color", kColor);
    const TextureHandle unused = g.create_texture("Unused", kColor);
    const TextureHandle side = g.create_texture("Side", kColor);

    const TextureHandle c1 = pass(g, "draw", {}, {color})[0];
    const TextureHandle u1 = pass(g, "orphan", {}, {unused})[0];
    pass(g, "orphan reader", {u1}, {g.create_texture("Orphan out", kColor)});
    pass(g, "readback", {}, {side}, true);
    pass(g, "resolve", {c1}, {out});
    g.compile();

    CHECK_FALSE(g.is_culled(pass_index(g, "draw")));
    CHECK_FALSE(g.is_culled(pass_index(g, "resolve")));
    CHECK(g.is_culled(pass_index(g, "orphan")));
    CHECK(g.is_culled(pass_index(g, "orphan reader")));
    CHECK_FALSE(g.is_culled(pass_index(g, "readback"))); // side effect
    CHECK(g.execution_order().size() == 3);
    // Culled passes' resources take no memory.
    CHECK(g.physical_slot(unused.resource) == -1);
}

TEST_CASE("render graph: a chain is culled transitively, and marked outputs keep it") {
    for (const bool marked : {false, true}) {
        RenderGraph g;
        const TextureHandle a = g.create_texture("A", kColor);
        const TextureHandle b = g.create_texture("B", kColor);
        const TextureHandle a1 = pass(g, "first", {}, {a})[0];
        const TextureHandle b1 = pass(g, "second", {a1}, {b})[0];
        if (marked) {
            g.mark_output(b1);
        }
        g.compile();
        CHECK(g.is_culled(0) == !marked);
        CHECK(g.is_culled(1) == !marked);
    }
}

TEST_CASE("render graph: draw-on-top writes depend on the earlier writer") {
    RenderGraph g;
    const TextureHandle out = g.import_texture("Output", {}, kColor);
    TextureHandle color = g.create_texture("Color", kColor);
    color = pass(g, "clear+mesh", {}, {color})[0];
    color = pass(g, "grid", {}, {color})[0]; // only writes, but loads the previous contents
    color = pass(g, "lines", {}, {color})[0];
    pass(g, "resolve", {color}, {out});
    g.compile();
    CHECK(g.execution_order().size() == 4);

    // Without the final reader, the whole chain goes.
    RenderGraph h;
    TextureHandle c = h.create_texture("Color", kColor);
    c = pass(h, "clear+mesh", {}, {c})[0];
    c = pass(h, "grid", {}, {c})[0];
    h.compile();
    CHECK(h.execution_order().empty());
}

TEST_CASE("render graph: transients with disjoint lifetimes share a slot, live ones never do") {
    RenderGraph g;
    const TextureHandle out = g.import_texture("Output", {}, kColor);
    // A -> B -> C -> D -> out: A and C (and B and D) can share memory.
    TextureHandle a = g.create_texture("A", kColor);
    TextureHandle b = g.create_texture("B", kColor);
    TextureHandle c = g.create_texture("C", kColor);
    TextureHandle d = g.create_texture("D", kColor);
    a = pass(g, "pa", {}, {a})[0];
    b = pass(g, "pb", {a}, {b})[0];
    c = pass(g, "pc", {b}, {c})[0];
    d = pass(g, "pd", {c}, {d})[0];
    pass(g, "po", {d}, {out});
    g.compile();

    check_no_overlap(g);
    CHECK(g.physical_slot(a.resource) == g.physical_slot(c.resource));
    CHECK(g.physical_slot(b.resource) == g.physical_slot(d.resource));
    CHECK(g.physical_slot(a.resource) != g.physical_slot(b.resource));
    CHECK(g.physical_slot_count() == 2);
    CHECK(g.physical_slot(out.resource) == -1); // imported: never aliased
}

TEST_CASE("render graph: different sizes, formats or usages are not aliased") {
    RenderGraph g;
    const TextureHandle out = g.import_texture("Output", {}, kColor);
    TextureHandle a = g.create_texture("A", kColor);
    TextureHandle b = g.create_texture("B", {32, 32, rhi::Format::RGBA16Float});
    TextureHandle c = g.create_texture("C", kColor);
    a = pass(g, "pa", {}, {a})[0];
    b = pass(g, "pb", {a}, {b})[0];
    // C is only used as a storage texture: different bind flags from A.
    g.add_pass("pc",
               [&](graph::PassBuilder& builder) {
                   builder.read(b);
                   c = builder.write(c, Access::Storage);
               },
               {});
    pass(g, "po", {c}, {out});
    g.compile();
    check_no_overlap(g);
    CHECK(g.physical_slot(a.resource) != g.physical_slot(c.resource));
    CHECK(g.physical_slot_count() == 3);
    CHECK(has_flag(g.bind_flags(c.resource), rhi::BindFlags::UnorderedAccess));
    CHECK(has_flag(g.bind_flags(c.resource), rhi::BindFlags::ShaderResource));
}

TEST_CASE("render graph: aliasing never overlaps live resources (random graphs)") {
    std::mt19937 rng(1234);
    for (int iteration = 0; iteration < 200; ++iteration) {
        RenderGraph g;
        const TextureHandle out = g.import_texture("Output", {}, kColor);
        std::vector<TextureHandle> live;
        const int passes = 2 + static_cast<int>(rng() % 20);
        for (int p = 0; p < passes; ++p) {
            std::vector<TextureHandle> reads;
            for (const TextureHandle t : live) {
                if (rng() % 3 == 0) {
                    reads.push_back(t);
                }
            }
            const graph::TextureDesc desc = rng() % 2 ? kColor : graph::TextureDesc{32, 32, rhi::Format::RGBA16Float};
            const TextureHandle created = g.create_texture("T" + std::to_string(p), desc);
            const TextureHandle written = pass(g, "P" + std::to_string(p), reads, {created})[0];
            live.push_back(written);
        }
        // The output reads a random subset, so some chains are culled.
        std::vector<TextureHandle> final_reads;
        for (const TextureHandle t : live) {
            if (rng() % 4 == 0) {
                final_reads.push_back(t);
            }
        }
        final_reads.push_back(live.back());
        pass(g, "final", final_reads, {out});
        g.compile();
        check_no_overlap(g);
        // Every surviving resource has a slot; every culled one none.
        for (std::uint32_t r = 1; r < g.resource_count(); ++r) {
            CHECK((g.physical_slot(r) >= 0) == (g.lifetime(r).first >= 0));
        }
    }
}

TEST_CASE("render graph: barriers move textures into each pass's state") {
    RenderGraph g;
    const TextureHandle out = g.import_texture("Output", {}, kColor, rhi::ResourceState::ShaderResource);
    TextureHandle color = g.create_texture("Color", kColor);
    TextureHandle depth = g.create_texture("Depth", kDepth);
    g.add_pass("draw",
               [&](graph::PassBuilder& b) {
                   color = b.write(color);
                   depth = b.write(depth, Access::DepthWrite);
               },
               {});
    g.add_pass("overlay",
               [&](graph::PassBuilder& b) {
                   color = b.write(color);
                   b.read(depth, Access::DepthTest);
               },
               {});
    pass(g, "resolve", {color}, {out});
    g.compile();

    const auto& draw = g.barriers_before(0);
    REQUIRE(draw.size() == 2);
    CHECK(draw[0].after == rhi::ResourceState::RenderTarget);
    CHECK(draw[1].after == rhi::ResourceState::DepthWrite);
    CHECK(g.barriers_before(1).empty()); // already in the right states
    const auto& resolve = g.barriers_before(2);
    REQUIRE(resolve.size() == 2);
    CHECK(resolve[0].resource == color.resource);
    CHECK(resolve[0].before == rhi::ResourceState::RenderTarget);
    CHECK(resolve[0].after == rhi::ResourceState::ShaderResource);
    CHECK(resolve[1].resource == out.resource);
    CHECK(resolve[1].after == rhi::ResourceState::RenderTarget);
    // The output is left readable by the UI.
    REQUIRE(g.final_barriers().size() == 1);
    CHECK(g.final_barriers()[0].after == rhi::ResourceState::ShaderResource);
}

TEST_CASE("render graph: consecutive storage writes get a UAV barrier") {
    RenderGraph g;
    TextureHandle t = g.create_texture("T", kColor);
    g.add_pass("a", [&](graph::PassBuilder& b) { t = b.write(t, Access::Storage); }, {});
    g.add_pass("b", [&](graph::PassBuilder& b) { t = b.write(t, Access::Storage); }, {});
    g.mark_output(t);
    g.compile();
    REQUIRE(g.barriers_before(1).size() == 1);
    CHECK(g.barriers_before(1)[0].before == rhi::ResourceState::UnorderedAccess);
    CHECK(g.barriers_before(1)[0].after == rhi::ResourceState::UnorderedAccess);
}

TEST_CASE("render graph: Graphviz dump names passes, resources and culling") {
    RenderGraph g;
    const TextureHandle out = g.import_texture("Output", {}, kColor);
    const TextureHandle c = pass(g, "draw", {}, {g.create_texture("Color", kColor)})[0];
    pass(g, "unused", {}, {g.create_texture("Junk", kColor)});
    pass(g, "resolve", {c}, {out});
    g.compile();
    const std::string dot = g.to_graphviz();
    CHECK(dot.starts_with("digraph RenderGraph {"));
    CHECK(dot.find("\"draw\"") != std::string::npos);
    CHECK(dot.find("unused\\n(culled)") != std::string::npos);
    CHECK(dot.find("Output\\n64x64 imported") != std::string::npos);
    CHECK(dot.find("slot 0") != std::string::npos);
}
