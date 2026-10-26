// SPDX-License-Identifier: Apache-2.0
#pragma once

// The embedded Python interpreter and the `forge3d` module (phases.md P5.4).
//
//   import forge3d
//   rock = forge3d.ops.entity.create(name="Rock")        # every operation, generated from ops.list
//   forge3d.ops.entity.set_property(entity=..., path="Transform.translation", value=[0, 1, 0])
//   forge3d.undo(); forge3d.redo()
//   forge3d.scene_query(component="MeshRenderer")       # the RPC methods, in process
//   forge3d.capture("shot.png", width=1280, height=720)
//   forge3d.call("entity.get", {"entity": guid})         # any RPC method by name
//
// The module calls the same dispatcher as the JSON-RPC server (api::RpcDispatcher),
// in process, so scripts and remote clients behave identically. Errors raise
// forge3d.Error (with .code, the JSON-RPC error code).

#include "f3d/api/rpc.hpp"

#include <filesystem>
#include <string>

namespace f3d::python {

/// What a script run produced.
struct RunResult {
    bool ok = false;
    std::string output; ///< captured stdout and stderr (when capturing)
    std::string error;  ///< the exception and traceback, if it failed
};

/// The process's Python interpreter (one per process).
class Interpreter {
public:
    explicit Interpreter(api::RpcServices services);
    ~Interpreter();
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    /// Starts Python with the forge3d module. False (and `error`) on failure.
    bool start(std::string* error = nullptr);
    [[nodiscard]] bool running() const noexcept { return running_; }

    /// Runs a script file as __main__. `capture`: collect its output instead of
    /// letting it reach the process's stdout/stderr.
    RunResult run_file(const std::filesystem::path& path, bool capture);
    /// Runs console input: an expression's value is echoed (like the Python prompt).
    RunResult run_string(const std::string& code);

private:
    api::RpcServices services_;
    bool running_ = false;
};

/// The Python version this build embeds ("3.14.8").
[[nodiscard]] std::string version();

} // namespace f3d::python
