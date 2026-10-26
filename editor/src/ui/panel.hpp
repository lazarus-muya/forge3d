// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f3d::editor {

/// A dockable editor window. Subclasses implement draw_contents(); the base
/// handles the ImGui window, its stable ID and the open/closed state.
class Panel {
public:
    Panel(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {}
    virtual ~Panel() = default;
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    /// ImGui window name: visible title + stable ID ("Console###console").
    [[nodiscard]] std::string window_name() const { return title_ + "###" + id_; }

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    void set_open(bool open) noexcept { open_ = open; }
    bool* open_flag() noexcept { return &open_; }

    /// Draws the panel's window if it is open.
    void draw();

protected:
    virtual void draw_contents() = 0;
    /// Panels that fill their window edge to edge (the viewport) return true.
    [[nodiscard]] virtual bool zero_padding() const { return false; }

private:
    std::string id_;
    std::string title_;
    bool open_ = true;
};

/// Registry of the editor's panels, in registration order.
class PanelManager {
public:
    template <class T, class... Args>
    T& add(Args&&... args) {
        auto panel = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *panel;
        panels_.push_back(std::move(panel));
        return ref;
    }

    [[nodiscard]] Panel* find(std::string_view id) const;
    void draw_all();
    [[nodiscard]] const std::vector<std::unique_ptr<Panel>>& panels() const noexcept { return panels_; }

private:
    std::vector<std::unique_ptr<Panel>> panels_;
};

} // namespace f3d::editor
