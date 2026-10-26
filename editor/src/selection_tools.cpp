// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/selection_tools.hpp"

#include <algorithm>

namespace f3d::editor {
namespace {

bool contains(const std::vector<core::Guid>& list, const core::Guid& guid) {
    return std::find(list.begin(), list.end(), guid) != list.end();
}

} // namespace

SelectionResult combine_selection(const std::vector<core::Guid>& current, const core::Guid& active,
                                  const std::vector<core::Guid>& picked, SelectMode mode) {
    SelectionResult out;
    switch (mode) {
    case SelectMode::Replace:
        for (const core::Guid& guid : picked) {
            if (!contains(out.entities, guid)) {
                out.entities.push_back(guid);
            }
        }
        out.active = out.entities.empty() ? core::Guid{} : out.entities.front();
        return out;
    case SelectMode::Add:
        out.entities = current;
        for (const core::Guid& guid : picked) {
            if (!contains(out.entities, guid)) {
                out.entities.push_back(guid);
            }
        }
        break;
    case SelectMode::Remove:
        for (const core::Guid& guid : current) {
            if (!contains(picked, guid)) {
                out.entities.push_back(guid);
            }
        }
        break;
    case SelectMode::Toggle:
        out.entities = current;
        for (const core::Guid& guid : picked) {
            const auto it = std::find(out.entities.begin(), out.entities.end(), guid);
            if (it == out.entities.end()) {
                out.entities.push_back(guid);
            } else {
                out.entities.erase(it);
            }
        }
        break;
    }
    // The newly picked entity becomes active if it is selected; otherwise keep the old one.
    for (const core::Guid& guid : picked) {
        if (contains(out.entities, guid)) {
            out.active = guid;
            return out;
        }
    }
    out.active = contains(out.entities, active) ? active : (out.entities.empty() ? core::Guid{} : out.entities.front());
    return out;
}

} // namespace f3d::editor
