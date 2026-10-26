// SPDX-License-Identifier: Apache-2.0
#pragma once

// Operation scripts: a JSON list of operations to run in order (phases.md §0.6
// `--scene <script>`, forge3d-cli). Format:
//
//   { "format": "forge3d.ops", "version": 1,
//     "ops": [ { "op": "entity.create", "params": { "name": "Rock" } }, ... ] }
//
// A bare array of { "op", "params" } objects is accepted too.

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

namespace f3d::ops {

class OpRegistry;
class OpStack;

struct ScriptResult {
    bool ok = false;
    std::size_t executed = 0; ///< operations that ran successfully
    std::string error;        ///< the first failure ("op #3 entity.delete: ...")
};

/// Runs every operation of `script` on `stack`, stopping at the first failure.
/// With `transaction`, they are recorded as one undo step named after it.
ScriptResult run_script(OpStack& stack, const OpRegistry& registry, const nlohmann::json& script,
                        const std::string& transaction = {});

/// Loads a script file and runs it (see run_script).
ScriptResult run_script_file(OpStack& stack, const OpRegistry& registry, const std::filesystem::path& path,
                             const std::string& transaction = {});

} // namespace f3d::ops
