// SPDX-License-Identifier: Apache-2.0
// The JSON-RPC dispatcher without sockets (phases.md P5.2): the JSON-RPC 2.0 rules,
// authentication, methods and events.
#include <doctest/doctest.h>

#include "f3d/api/rpc.hpp"
#include "f3d/ops/builtin.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"
#include "f3d/scene/reflect.hpp"
#include "f3d/scene/world.hpp"
#include "f3d/terrain/terrain_ops.hpp"

using namespace f3d;
using nlohmann::json;

namespace {

struct Fixture {
    scene::World world;
    ops::ChangeBus changes;
    ops::Context context{world, changes};
    ops::OpStack stack{context};
    ops::OpRegistry registry;
    api::Host host{registry, stack, world};
    api::RpcDispatcher dispatcher;
    api::RpcSession session;

    Fixture() : dispatcher(services(), "secret-token") {
        scene::register_types();
        ops::register_builtin_ops(registry);
    }

    api::RpcServices services() {
        api::RpcServices s;
        s.host = &host;
        s.changes = &changes;
        s.search_assets = [](const assets::AssetQuery& q) {
            std::vector<assets::AssetSummary> out;
            if (q.text == "rock") {
                out.push_back({core::Guid{1, 2}, "project://rock.glb", "rock", "model", false});
            }
            return out;
        };
        return s;
    }

    json send(const json& message) {
        const auto reply = dispatcher.handle(message.dump(), session);
        return reply ? json::parse(*reply) : json();
    }
    json call(const std::string& method, const json& params = json::object(), int id = 1) {
        return send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    }
    void authenticate() { REQUIRE(call("session.authenticate", {{"token", "secret-token"}}).contains("result")); }
};

} // namespace

TEST_CASE("rpc: authentication comes first") {
    Fixture f;
    json reply = f.call("ops.list");
    CHECK(reply["error"]["code"] == api::kNotAuthenticated);
    CHECK(f.session.closing);

    Fixture g;
    reply = g.call("session.authenticate", {{"token", "secret-tokeN"}});
    CHECK(reply["error"]["code"] == api::kNotAuthenticated);
    CHECK(g.session.closing);
    CHECK_FALSE(g.session.authenticated);

    Fixture h;
    reply = h.call("session.authenticate", {{"token", "secret-token"}});
    CHECK(reply["result"]["authenticated"] == true);
    CHECK(h.session.authenticated);
    CHECK_FALSE(h.session.closing);

    // Garbage before authenticating also hangs up.
    Fixture k;
    const auto garbage = k.dispatcher.handle("{not json", k.session);
    REQUIRE(garbage.has_value());
    CHECK(json::parse(*garbage)["error"]["code"] == api::kParseError);
    CHECK(k.session.closing);
}

TEST_CASE("rpc: JSON-RPC 2.0 rules") {
    Fixture f;
    f.authenticate();
    // Wrong version, missing method.
    CHECK(f.send({{"jsonrpc", "1.0"}, {"id", 1}, {"method", "ops.list"}})["error"]["code"] == api::kInvalidRequest);
    CHECK(f.send({{"jsonrpc", "2.0"}, {"id", 2}})["error"]["code"] == api::kInvalidRequest);
    // Unknown method, bad params.
    CHECK(f.call("nope")["error"]["code"] == api::kMethodNotFound);
    CHECK(f.send({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ops.run"}, {"params", json::array()}})["error"]["code"] ==
          api::kInvalidParams);
    CHECK(f.call("ops.run", {{"params", {}}})["error"]["code"] == api::kInvalidParams); // no id
    // The id is echoed (string ids too).
    CHECK(f.call("ops.undo", json::object(), 42)["id"] == 42);
    CHECK(f.send({{"jsonrpc", "2.0"}, {"id", "abc"}, {"method", "ops.undo"}})["id"] == "abc");
    // A notification (no id) gets no reply, but runs.
    CHECK_FALSE(f.dispatcher
                    .handle(json{{"jsonrpc", "2.0"},
                                 {"method", "ops.run"},
                                 {"params", {{"id", "entity.create"}, {"params", {{"name", "Quiet"}}}}}}
                                .dump(),
                            f.session)
                    .has_value());
    CHECK(f.world.size() == 1);
    // A batch answers every request in it.
    const json batch = f.send(json::array({{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "scene.query"}},
                                           {{"jsonrpc", "2.0"}, {"method", "ops.undo"}},
                                           {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "nope"}}}));
    REQUIRE(batch.is_array());
    CHECK(batch.size() == 2);
    CHECK(batch[0]["result"].size() == 1);
    CHECK(batch[1]["error"]["code"] == api::kMethodNotFound);
    CHECK(f.send(json::array())["error"]["code"] == api::kInvalidRequest);
}

TEST_CASE("rpc: operations, scene and entity methods") {
    Fixture f;
    f.authenticate();
    const std::string guid = "00000000-0000-4000-8000-00000000aa01";
    REQUIRE(f.call("ops.run", {{"id", "entity.create"}, {"params", {{"name", "Rock"}, {"guid", guid}}}})
                .contains("result"));
    json reply = f.call("entity.set", {{"entity", guid}, {"path", "Transform.translation"}, {"value", {1, 2, 3}}});
    CHECK(reply["result"]["undo_steps"] == 2);
    reply = f.call("entity.get", {{"entity", guid}});
    CHECK(reply["result"]["name"] == "Rock");
    CHECK(reply["result"]["components"]["Transform"]["translation"] == json::array({1.0, 2.0, 3.0}));
    CHECK(f.call("entity.get", {{"entity", "00000000-0000-4000-8000-000000000000"}})["error"]["code"] ==
          api::kInvalidParams);
    CHECK(f.call("scene.query", {{"name", "ROCK"}})["result"].size() == 1); // case-insensitive
    CHECK(f.call("scene.query", {{"component", "Light"}})["result"].empty());
    CHECK(f.call("selection.get")["result"]["entities"].empty());
    REQUIRE(f.call("ops.run", {{"id", "selection.set"}, {"params", {{"entities", {guid}}, {"active", guid}}}})
                .contains("result"));
    CHECK(f.call("selection.get")["result"]["entities"] == json::array({guid}));
    CHECK(f.call("selection.get")["result"]["active"] == guid);
    REQUIRE(f.call("ops.undo")["result"]["done"] == true);
    CHECK(f.call("ops.run", {{"id", "entity.delete"}, {"params", {{"entity", guid}}}})["result"]["undo_steps"] == 3);
    CHECK(f.call("ops.undo")["result"]["done"] == true);
    CHECK(f.world.size() == 1);
    CHECK(f.call("ops.redo")["result"]["done"] == true);
    CHECK(f.world.size() == 0);
    CHECK(f.call("ops.run", {{"id", "entity.delete"}, {"params", {{"entity", guid}}}})["error"]["code"] ==
          api::kOperationFailed);
    CHECK(f.call("ops.run", {{"id", "no.such_op"}})["error"]["code"] == api::kInvalidParams);

    const json list = f.call("ops.list")["result"];
    const auto create =
        std::find_if(list.begin(), list.end(), [](const json& op) { return op["id"] == "entity.create"; });
    REQUIRE(create != list.end());
    CHECK((*create)["schema"]["properties"]["name"]["type"] == "string");
    CHECK((*create)["schema"]["properties"]["parent"]["format"] == "uuid");

    // Assets come from the host's search; captures need a renderer.
    CHECK(f.call("asset.search", {{"text", "rock"}})["result"][0]["path"] == "project://rock.glb");
    CHECK(f.call("viewport.capture")["error"]["code"] == api::kUnavailable);
}

TEST_CASE("rpc: events reach subscribed sessions only, filtered by kind") {
    Fixture f;
    f.authenticate();
    const ops::ChangeEvent created{ops::ChangeKind::EntityCreated, core::Guid{3, 4}, {}};
    const ops::ChangeEvent asset{ops::ChangeKind::AssetChanged, core::Guid{}, "project://m.f3dmat"};
    CHECK_FALSE(f.dispatcher.event_for(created, f.session).has_value()); // not subscribed
    REQUIRE(f.call("events.subscribe", {{"kinds", {"entity_created"}}}).contains("result"));
    const auto text = f.dispatcher.event_for(created, f.session);
    REQUIRE(text.has_value());
    const json event = json::parse(*text);
    CHECK(event["method"] == "events.changed");
    CHECK_FALSE(event.contains("id"));
    CHECK(event["params"]["kind"] == "entity_created");
    CHECK(event["params"]["entity"] == core::Guid{3, 4}.to_string());
    CHECK_FALSE(f.dispatcher.event_for(asset, f.session).has_value()); // filtered out
    REQUIRE(f.call("events.subscribe").contains("result"));            // all kinds
    CHECK(json::parse(*f.dispatcher.event_for(asset, f.session))["params"]["asset"] == "project://m.f3dmat");
}

TEST_CASE("rpc: JSON Schema from parameter rows, tokens") {
    const json schema =
        api::json_schema(json::array({{{"name", "count"}, {"type", "uint32"}},
                                      {{"name", "where"}, {"type", "vec3"}},
                                      {{"name", "speed"}, {"type", "float"}, {"min", 0.0}, {"max", 2.0}},
                                      {{"name", "mesh"}, {"type", "asset_ref"}},
                                      {{"name", "extra"}, {"type", "json"}, {"tooltip", "anything"}}}));
    CHECK(schema["type"] == "object");
    CHECK(schema["properties"]["count"]["minimum"] == 0);
    CHECK(schema["properties"]["where"]["maxItems"] == 3);
    CHECK(schema["properties"]["speed"]["maximum"] == 2.0);
    CHECK(schema["properties"]["mesh"]["type"] == "object");
    CHECK(schema["properties"]["extra"]["description"] == "anything");
    CHECK_FALSE(schema["properties"]["extra"].contains("type"));

    const std::string a = api::generate_token();
    const std::string b = api::generate_token();
    CHECK(a.size() == 64);
    CHECK(a != b);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("rpc: terrain.info and terrain.sample read a sculpted terrain") {
    Fixture f;
    terrain::register_terrain_ops(f.registry);
    f.authenticate();
    const std::string guid = "00000000-0000-4000-8000-00000000aa21";
    REQUIRE(f.call("ops.run", {{"id", "terrain.create"},
                               {"params", {{"guid", guid}, {"tiles", 1}, {"size", 256.0}, {"max_height", 100.0}}}})
                .contains("result"));
    // Move it: samples are in world space.
    REQUIRE(f.call("entity.set", {{"entity", guid}, {"path", "Transform.translation"}, {"value", {100, 5, 0}}})
                .contains("result"));
    const json info = f.call("terrain.info", {{"entity", guid}})["result"];
    CHECK(info["samples"] == 257);
    CHECK(info["size"] == 256.0);
    CHECK(info["origin"] == json::array({100.0, 5.0, 0.0}));
    CHECK(info["hash"].get<std::string>().size() == 16);
    const double before = f.call("terrain.sample", {{"entity", guid}, {"points", json::array({json::array({100, 0})})}})["result"]["heights"][0];

    REQUIRE(f.call("ops.run", {{"id", "terrain.sculpt_stroke"},
                               {"params",
                                {{"terrain", guid},
                                 {"tool", "flatten"},
                                 {"use_target", true},
                                 {"target_height", 42.0},
                                 {"strength", 1.0},
                                 {"radius", 20.0},
                                 {"points", json::array({json::array({0.0, 0.0, 1.0})})}}}})
                .contains("result"));
    const json after = f.call("terrain.sample", {{"entity", guid}, {"points", json::array({json::array({100, 0}), json::array({300, 0})})}})["result"];
    CHECK(after["heights"][0].get<double>() == doctest::Approx(42.0 + 5.0));
    CHECK(f.call("terrain.info", {{"entity", guid}})["result"]["hash"] != info["hash"]);

    REQUIRE(f.call("ops.undo").contains("result"));
    CHECK(f.call("terrain.info", {{"entity", guid}})["result"]["hash"] == info["hash"]);
    CHECK(f.call("terrain.sample", {{"entity", guid}, {"points", json::array({json::array({100, 0})})}})["result"]["heights"][0] == before);

    CHECK(f.call("terrain.sample", {{"entity", guid}, {"points", json::array({json::array({1})})}})["error"]["code"] == api::kInvalidParams);
    CHECK(f.call("terrain.sample", {{"entity", guid}})["error"]["code"] == api::kInvalidParams);
    REQUIRE(f.call("ops.run", {{"id", "entity.create"}, {"params", {{"guid", "00000000-0000-4000-8000-00000000aa22"}}}})
                .contains("result"));
    CHECK(f.call("terrain.info", {{"entity", "00000000-0000-4000-8000-00000000aa22"}})["error"]["code"] ==
          api::kInvalidParams);
}
