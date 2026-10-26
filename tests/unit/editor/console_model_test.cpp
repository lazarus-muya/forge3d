// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/core/log.hpp"
#include "f3d/editor/command_registry.hpp"
#include "f3d/editor/console_model.hpp"

#include <memory>

using f3d::core::LogLevel;
using f3d::editor::ConsoleModel;

TEST_CASE("console model: receives log calls through the log sink") {
    auto model = std::make_shared<ConsoleModel>();
    const LogLevel saved = f3d::core::log::level();
    f3d::core::log::set_console_enabled(false);
    f3d::core::log::set_level(LogLevel::Trace);
    f3d::core::log::add_sink(model);

    F3D_LOG_INFO("Imported {} meshes", 3);
    F3D_LOG_WARN("Texture missing: rock.png");
    F3D_LOG_DEBUG("debug detail");

    f3d::core::log::remove_sink(model);
    f3d::core::log::set_level(saved);
    f3d::core::log::set_console_enabled(true);

    REQUIRE(model->size() == 3);
    const auto all = model->filtered(LogLevel::Trace, "");
    REQUIRE(all.size() == 3);
    CHECK(all[0].message == "Imported 3 meshes");
    CHECK(all[0].level == LogLevel::Info);
    CHECK(all[0].time.size() == 12); // HH:MM:SS.mmm
    CHECK(all[1].level == LogLevel::Warn);
}

TEST_CASE("console model: level filter and case-insensitive search") {
    ConsoleModel model;
    const auto now = std::chrono::system_clock::now();
    model.write({LogLevel::Debug, now, "loading shaders"});
    model.write({LogLevel::Info, now, "Shader hot reload: grid.ps recompiled"});
    model.write({LogLevel::Error, now, "SHADER compile failed"});

    CHECK(model.filtered(LogLevel::Info, "").size() == 2);
    CHECK(model.filtered(LogLevel::Error, "").size() == 1);
    CHECK(model.filtered(LogLevel::Trace, "shader").size() == 3);
    CHECK(model.filtered(LogLevel::Info, "shader").size() == 2);
    CHECK(model.filtered(LogLevel::Trace, "grid.ps").size() == 1);
    CHECK(model.filtered(LogLevel::Trace, "nothing").empty());

    const auto v = model.version();
    model.clear();
    CHECK(model.size() == 0);
    CHECK(model.version() > v);
}

TEST_CASE("console model: bounded history drops the oldest lines") {
    ConsoleModel model(3);
    const auto now = std::chrono::system_clock::now();
    for (int i = 0; i < 5; ++i) {
        model.write({LogLevel::Info, now, std::to_string(i)});
    }
    const auto all = model.filtered(LogLevel::Trace, "");
    REQUIRE(all.size() == 3);
    CHECK(all.front().message == "2");
    CHECK(all.back().message == "4");
}

TEST_CASE("command registry: add, replace, execute") {
    f3d::editor::CommandRegistry registry;
    int calls = 0;
    CHECK(registry.add({"editor.reset_layout", "Reset Layout", [&] { ++calls; }}));
    CHECK_FALSE(registry.add({"", "No id", [] {}}));
    CHECK(registry.execute("editor.reset_layout"));
    CHECK(calls == 1);
    CHECK_FALSE(registry.execute("editor.missing"));

    CHECK(registry.add({"editor.reset_layout", "Reset Layout v2", [&] { calls += 10; }}));
    CHECK(registry.commands().size() == 1);
    CHECK(registry.find("editor.reset_layout")->name == "Reset Layout v2");
    registry.execute("editor.reset_layout");
    CHECK(calls == 11);
}
