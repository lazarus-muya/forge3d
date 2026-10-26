// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "f3d/core/guid.hpp"

#include <compare>
#include <string>

namespace f3d::core {

/// Reference to an asset (or a sub-asset such as one mesh of a glTF file).
///
/// `path` is the source path with an optional `#<sub-asset>` suffix, e.g.
/// "C:/models/Fox.glb#mesh/0". `guid` is the asset's identity once the asset
/// database exists (M3); until then it is null and `path` alone resolves it.
struct AssetRef {
    Guid guid;
    std::string path;

    [[nodiscard]] bool empty() const noexcept { return guid.is_null() && path.empty(); }
    friend bool operator==(const AssetRef&, const AssetRef&) = default;
    friend std::strong_ordering operator<=>(const AssetRef&, const AssetRef&) = default;
};

} // namespace f3d::core
