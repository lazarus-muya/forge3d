// SPDX-License-Identifier: Apache-2.0
#include "f3d/ops/script.hpp"

#include "f3d/core/profile.hpp"
#include "f3d/ops/registry.hpp"
#include "f3d/ops/stack.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <memory>

namespace f3d::ops {

ScriptResult run_script(OpStack& stack, const OpRegistry& registry, const nlohmann::json& script,
                        const std::string& transaction) {
    F3D_ZONE;
    ScriptResult result;
    const nlohmann::json* list = &script;
    if (script.is_object()) {
        if (script.value("format", std::string()) != "forge3d.ops") {
            result.error = "not a forge3d.ops script (\"format\" must be \"forge3d.ops\")";
            return result;
        }
        if (script.value("version", 0) != 1) {
            result.error = "unsupported forge3d.ops version (expected 1)";
            return result;
        }
        if (!script.contains("ops")) {
            result.error = "missing \"ops\"";
            return result;
        }
        list = &script.at("ops");
    }
    if (!list->is_array()) {
        result.error = "\"ops\" must be an array";
        return result;
    }

    if (!transaction.empty()) {
        stack.begin_transaction(transaction);
    }
    std::size_t index = 0;
    for (const nlohmann::json& entry : *list) {
        ++index;
        const std::string id = entry.is_object() ? entry.value("op", std::string()) : std::string();
        const auto fail = [&](const std::string& why) {
            result.error = "op #" + std::to_string(index) + (id.empty() ? "" : " " + id) + ": " + why;
        };
        if (id.empty()) {
            fail("missing \"op\"");
            break;
        }
        std::unique_ptr<Operation> op;
        try {
            op = registry.create(id, entry.contains("params") ? entry.at("params") : nlohmann::json::object());
        } catch (const std::exception& e) {
            fail(std::string("bad parameters: ") + e.what());
            break;
        }
        if (!op) {
            fail("unknown operation");
            break;
        }
        if (!stack.execute(std::move(op))) {
            fail("failed (invalid parameters for the current scene)");
            break;
        }
        ++result.executed;
    }
    result.ok = result.error.empty();
    if (!transaction.empty()) {
        if (result.ok) {
            stack.end_transaction();
        } else {
            stack.abort_transaction(); // all or nothing
        }
    }
    return result;
}

ScriptResult run_script_file(OpStack& stack, const OpRegistry& registry, const std::filesystem::path& path,
                             const std::string& transaction) {
    ScriptResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "cannot open " + path.string();
        return result;
    }
    nlohmann::json script;
    try {
        in >> script;
    } catch (const std::exception& e) {
        result.error = path.string() + ": " + e.what();
        return result;
    }
    return run_script(stack, registry, script, transaction);
}

} // namespace f3d::ops
