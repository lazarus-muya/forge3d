// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "panel.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace f3d::editor {

namespace panel_ids {
inline constexpr const char* kPython = "python";
} // namespace panel_ids

/// The result of running Python code (output and, on failure, the traceback).
struct PythonOutput {
    bool ok = false;
    std::string text;
};

/// What the Python panel asks the App (phases.md P5.4).
struct PythonActions {
    bool built = false;            ///< this build embeds Python
    std::function<bool()> enabled; ///< the user allowed scripts (Preferences ▸ Automation)
    std::function<PythonOutput(const std::string& code)> run_code;
    std::function<PythonOutput(const std::string& path)> run_file;
};

/// Python console and script runner. Nothing runs unless Python scripts are enabled
/// in the preferences (a separate permission from the JSON-RPC server).
class PythonPanel final : public Panel {
public:
    explicit PythonPanel(PythonActions actions);

protected:
    void draw_contents() override;

private:
    struct Entry {
        std::string text;
        bool input = false;
        bool error = false;
    };
    void run_input();

    PythonActions actions_;
    std::vector<Entry> history_;
    std::array<char, 4096> input_{};
    std::array<char, 1024> script_{};
    bool scroll_to_bottom_ = false;
};

} // namespace f3d::editor
