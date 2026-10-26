// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/editor.hpp"

#include "app.hpp"
#include "f3d/core/log.hpp"
#include "f3d/editor/launch_options.hpp"

#include <cstdio>
#include <string_view>
#include <vector>

namespace f3d::editor {

int run(int argc, char** argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const LaunchParseResult parsed = parse_launch_options(args);
    if (!parsed.ok()) {
        std::fprintf(stderr, "forge3d-editor: %s\n\n%.*s", parsed.error.c_str(),
                     static_cast<int>(launch_usage().size()), launch_usage().data());
        return 2;
    }
    if (parsed.options.help) {
        std::printf("%.*s", static_cast<int>(launch_usage().size()), launch_usage().data());
        return 0;
    }

    core::log::init(core::LogLevel::Info);
    int exit_code = 0;
    {
        App app(parsed.options);
        exit_code = app.run();
    }
    core::log::shutdown();
    return exit_code;
}

} // namespace f3d::editor
