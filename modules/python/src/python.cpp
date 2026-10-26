// SPDX-License-Identifier: Apache-2.0
// The embedded interpreter and the forge3d module (phases.md P5.4). See python.hpp.
#include "f3d/python/python.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/system.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

namespace nb = nanobind;

namespace f3d::python {
namespace {

/// The dispatcher the module talks to (set while the interpreter runs).
std::unique_ptr<api::RpcDispatcher> g_dispatcher;
api::RpcSession g_session;

nlohmann::json to_json(nb::handle object) {
    if (object.is_none()) {
        return nullptr;
    }
    if (nb::isinstance<nb::bool_>(object)) {
        return nb::cast<bool>(object);
    }
    if (nb::isinstance<nb::int_>(object)) {
        return nb::cast<long long>(object);
    }
    if (nb::isinstance<nb::float_>(object)) {
        return nb::cast<double>(object);
    }
    if (nb::isinstance<nb::str>(object)) {
        return nb::cast<std::string>(object);
    }
    if (nb::isinstance<nb::dict>(object)) {
        nlohmann::json out = nlohmann::json::object();
        for (auto [key, value] : nb::cast<nb::dict>(object)) {
            out[nb::cast<std::string>(nb::str(key))] = to_json(value);
        }
        return out;
    }
    if (nb::isinstance<nb::list>(object) || nb::isinstance<nb::tuple>(object)) {
        nlohmann::json out = nlohmann::json::array();
        for (nb::handle item : object) {
            out.push_back(to_json(item));
        }
        return out;
    }
    throw nb::type_error(("can't send a " + nb::cast<std::string>(nb::str(object.type())) + " to forge3d").c_str());
}

nb::object to_python(const nlohmann::json& value) {
    switch (value.type()) {
    case nlohmann::json::value_t::null:
    case nlohmann::json::value_t::discarded:
        return nb::none();
    case nlohmann::json::value_t::boolean:
        return nb::bool_(value.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return nb::int_(value.get<long long>());
    case nlohmann::json::value_t::number_unsigned:
        return nb::int_(value.get<unsigned long long>());
    case nlohmann::json::value_t::number_float:
        return nb::float_(value.get<double>());
    case nlohmann::json::value_t::string:
        return nb::str(value.get<std::string>().c_str());
    case nlohmann::json::value_t::array: {
        nb::list out;
        for (const auto& item : value) {
            out.append(to_python(item));
        }
        return out;
    }
    case nlohmann::json::value_t::object: {
        nb::dict out;
        for (const auto& [key, item] : value.items()) {
            out[nb::str(key.c_str())] = to_python(item);
        }
        return out;
    }
    case nlohmann::json::value_t::binary:
        break;
    }
    return nb::none();
}

/// forge3d.call(method, params): one JSON-RPC call through the dispatcher.
nb::object call(const std::string& method, nb::handle params) {
    if (!g_dispatcher) {
        throw std::runtime_error("forge3d isn't connected to a session");
    }
    nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}};
    if (!params.is_none()) {
        request["params"] = to_json(params);
    }
    const auto reply_text = g_dispatcher->handle(request.dump(), g_session);
    const nlohmann::json reply = nlohmann::json::parse(reply_text.value_or("{}"));
    if (reply.contains("error")) {
        nb::object module = nb::module_::import_("forge3d");
        nb::object error = module.attr("Error")(reply["error"].value("message", std::string("failed")),
                                                reply["error"].value("code", 0));
        PyErr_SetObject(error.type().ptr(), error.ptr());
        throw nb::python_error();
    }
    return to_python(reply.value("result", nlohmann::json()));
}

// Pure-Python parts of the module: the error type, the generated `ops` namespace and
// conveniences over call().
constexpr const char* kBootstrap = R"PY(
class Error(Exception):
    """A forge3d call failed. `code` is the JSON-RPC error code."""
    def __init__(self, message, code=0):
        super().__init__(message)
        self.code = code


class _Namespace:
    """forge3d.ops.<domain>: the operations of one domain."""
    def __init__(self, path):
        self._path = path
        self._children = {}

    def __getattr__(self, name):
        try:
            return self.__dict__["_children"][name]
        except KeyError:
            raise AttributeError("forge3d.ops has no " + ".".join(self._path + [name])) from None

    def __dir__(self):
        return sorted(self._children)

    def __repr__(self):
        return "<forge3d.ops" + ("." + ".".join(self._path) if self._path else "") + ">"


def _make_op(op):
    op_id = op["id"]

    def run(**params):
        return call("ops.run", {"id": op_id, "params": params})

    names = ", ".join(p["name"] for p in op["params"])
    run.__name__ = op_id.split(".")[-1]
    run.__qualname__ = op_id
    run.__doc__ = "%s (%s), undoable. Parameters: %s." % (op["name"], op_id, names or "none")
    return run


def refresh_ops():
    """Rebuilds forge3d.ops from the session's operations (plugins included)."""
    root = _Namespace([])
    for op in call("ops.list"):
        parts = op["id"].split(".")
        node = root
        for part in parts[:-1]:
            node = node._children.setdefault(part, _Namespace(node._path + [part]))
        node._children[parts[-1]] = _make_op(op)
    globals()["ops"] = root
    return root


def run_op(op_id, **params):
    """Runs an operation by id (undoable)."""
    return call("ops.run", {"id": op_id, "params": params})


def undo():
    return call("ops.undo")["done"]


def redo():
    return call("ops.redo")["done"]


def list_ops():
    return call("ops.list")


def scene_query(name=None, component=None):
    params = {}
    if name is not None:
        params["name"] = name
    if component is not None:
        params["component"] = component
    return call("scene.query", params)


def entity_get(entity):
    return call("entity.get", {"entity": str(entity)})


def terrain_info(entity):
    """A terrain's size, sample count, height range, layer count and hashes of its
    heights, its surface (with roads and riverbeds) and its painted layer weights."""
    return call("terrain.info", {"entity": str(entity)})


def terrain_sample(entity, points, surface=False):
    """Terrain heights (world) under [x, z] world points; with `surface`, of the ground
    as shaped by splines (roads, riverbeds) rather than the sculpted heights."""
    return call("terrain.sample", {"entity": str(entity), "points": [list(p) for p in points],
                                   "surface": bool(surface)})["heights"]


def selection():
    return call("selection.get")


def capture(path, width=1280, height=720, mode="lit", camera=None, bookmark=None):
    """Renders the scene to a PNG at `path`."""
    params = {"path": str(path), "width": width, "height": height, "mode": mode}
    if camera is not None:
        params["camera"] = str(camera)
    if bookmark is not None:
        params["bookmark"] = bookmark
    return call("viewport.capture", params)


def asset_search(text="", type="", tags=()):
    return call("asset.search", {"text": text, "type": type, "tags": list(tags)})
)PY";

constexpr const char* kCaptureStart = R"PY(
import io as _io, sys as _sys
_f3d_captured = _io.StringIO()
_f3d_saved = (_sys.stdout, _sys.stderr)
_sys.stdout = _sys.stderr = _f3d_captured
)PY";

constexpr const char* kCaptureEnd = R"PY(
import sys as _sys
_sys.stdout, _sys.stderr = _f3d_saved
)PY";

/// The traceback of the pending Python error, as text (clears it).
std::string format_exception() {
    PyObject* exception = PyErr_GetRaisedException();
    if (!exception) {
        return {};
    }
    std::string text;
    try {
        nb::object traceback = nb::module_::import_("traceback");
        nb::object lines = traceback.attr("format_exception")(nb::handle(exception));
        text = nb::cast<std::string>(nb::str("").attr("join")(lines));
    } catch (...) {
        text = "error (no traceback)";
    }
    Py_DECREF(exception);
    return text;
}

} // namespace

NB_MODULE(forge3d, m) {
    m.doc() = "forge3d: scripting the scene through its operations (phases.md P5.4)";
    m.def("call", &call, nb::arg("method"), nb::arg("params") = nb::none(),
          "Calls a JSON-RPC method of this session (see docs/architecture/api.md).");
    m.attr("__version__") = "0.1.0";
    nb::object globals = m.attr("__dict__");
    // Run the pure-Python part inside the module's own namespace.
    nb::object builtins = nb::module_::import_("builtins");
    builtins.attr("exec")(nb::str(kBootstrap), globals);
}

Interpreter::Interpreter(api::RpcServices services) : services_(std::move(services)) {}

Interpreter::~Interpreter() {
    if (running_) {
        Py_FinalizeEx();
        g_dispatcher.reset();
    }
}

bool Interpreter::start(std::string* error) {
    if (running_) {
        return true;
    }
    if (Py_IsInitialized()) {
        if (error) {
            *error = "Python is already running in this process";
        }
        return false;
    }
    g_dispatcher = std::make_unique<api::RpcDispatcher>(services_, "in-process");
    g_session = {};
    g_session.authenticated = true; // in process: the script is the user's own

    PyImport_AppendInittab("forge3d", &PyInit_forge3d);
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    // The standard library: next to the executable (packages), else vcpkg's (development).
    std::filesystem::path home = core::executable_dir() / "python";
    if (!std::filesystem::exists(home / "Lib")) {
        home = F3D_PYTHON_HOME;
    }
    PyStatus status = PyConfig_SetString(&config, &config.home, home.wstring().c_str());
    if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
    }
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        if (error) {
            *error = std::string("Python failed to start: ") + (status.err_msg ? status.err_msg : "?") + " (home " +
                     home.string() + ")";
        }
        g_dispatcher.reset();
        return false;
    }
    running_ = true;
    // Import forge3d with the plain C API first: that initializes nanobind, which the
    // helpers below (run_string) rely on.
    PyObject* module = PyImport_ImportModule("forge3d");
    if (!module) {
        PyErr_Print(); // nanobind isn't initialized: plain C API only
        if (error) {
            *error = "the forge3d module failed to load (traceback on stderr)";
        }
        return false;
    }
    PyObject_SetAttrString(PyImport_AddModule("__main__"), "forge3d", module);
    Py_DECREF(module);
    const RunResult r = run_string("forge3d.refresh_ops()\n");
    if (!r.ok) {
        if (error) {
            *error = "the forge3d module failed to load: " + r.error;
        }
        return false;
    }
    F3D_LOG_INFO("Python {} ready", version());
    return true;
}

RunResult Interpreter::run_file(const std::filesystem::path& path, bool capture) {
    RunResult result;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "cannot read " + path.string();
        return result;
    }
    std::stringstream source;
    source << file.rdbuf();
    nb::gil_scoped_acquire gil;
    nb::dict globals;
    globals["__name__"] = "__main__";
    globals["__file__"] = path.string();
    globals["__builtins__"] = nb::module_::import_("builtins");
    if (capture) {
        PyRun_String(kCaptureStart, Py_file_input, globals.ptr(), globals.ptr());
    }
    // Refresh the operations (plugins may have added some since start).
    nb::module_::import_("forge3d").attr("refresh_ops")();
    PyObject* code = Py_CompileString(source.str().c_str(), path.string().c_str(), Py_file_input);
    PyObject* value = code ? PyEval_EvalCode(code, globals.ptr(), globals.ptr()) : nullptr;
    Py_XDECREF(code);
    result.ok = value != nullptr;
    Py_XDECREF(value);
    if (!result.ok) {
        result.error = format_exception();
    }
    if (capture) {
        PyRun_String(kCaptureEnd, Py_file_input, globals.ptr(), globals.ptr());
        result.output = nb::cast<std::string>(globals["_f3d_captured"].attr("getvalue")());
    }
    return result;
}

RunResult Interpreter::run_string(const std::string& code) {
    RunResult result;
    nb::gil_scoped_acquire gil;
    nb::object main = nb::module_::import_("__main__");
    nb::object globals = main.attr("__dict__");
    PyRun_String(kCaptureStart, Py_file_input, globals.ptr(), globals.ptr());
    // An expression echoes its value (the interactive prompt's "single" mode); anything
    // else runs as statements.
    PyObject* compiled = Py_CompileString(code.c_str(), "<console>", Py_single_input);
    if (!compiled) {
        PyErr_Clear();
        compiled = Py_CompileString(code.c_str(), "<console>", Py_file_input);
    }
    PyObject* value = compiled ? PyEval_EvalCode(compiled, globals.ptr(), globals.ptr()) : nullptr;
    Py_XDECREF(compiled);
    result.ok = value != nullptr;
    Py_XDECREF(value);
    if (!result.ok) {
        result.error = format_exception();
    }
    PyRun_String(kCaptureEnd, Py_file_input, globals.ptr(), globals.ptr());
    result.output =
        nb::cast<std::string>(nb::handle(PyDict_GetItemString(globals.ptr(), "_f3d_captured")).attr("getvalue")());
    return result;
}

std::string version() {
    return PY_VERSION;
}

} // namespace f3d::python
