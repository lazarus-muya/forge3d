// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/ops/operation.hpp"

#include <entt/meta/meta.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3d::ops {

/// A registered operation's metadata (used by list-ops, the CLI and the RPC/MCP
/// schemas).
struct OpInfo {
    std::string id;
    std::string display_name;
    entt::meta_type params_type;
    /// Schema rows for parameters that aren't reflected fields (e.g. entity.set_property's
    /// free-form "value"), in param_schema's format.
    nlohmann::json extra_params = nlohmann::json::array();
};

/// Registry of operation types, looked up by id.
class OpRegistry {
public:
    using Factory = std::function<std::unique_ptr<Operation>()>;

    void add(OpInfo info, Factory factory);

    [[nodiscard]] const OpInfo* find(std::string_view id) const;
    [[nodiscard]] bool contains(std::string_view id) const { return find(id) != nullptr; }

    /// Builds an operation by id with its parameters set from JSON, or null.
    [[nodiscard]] std::unique_ptr<Operation> create(std::string_view id, const nlohmann::json& params) const;

    /// All registered operations, in registration order.
    [[nodiscard]] std::vector<OpInfo> list() const;
    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }

private:
    struct Entry {
        OpInfo info;
        Factory factory;
    };
    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> order_;
};

/// A JSON description of a reflected parameter struct's fields (name + type).
[[nodiscard]] nlohmann::json param_schema(entt::meta_type params_type);
/// An operation's full parameter schema: its reflected fields plus extra_params.
[[nodiscard]] nlohmann::json param_schema(const OpInfo& info);

} // namespace f3d::ops
