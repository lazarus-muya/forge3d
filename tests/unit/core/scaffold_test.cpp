// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include "f3d/api/module.hpp"

TEST_CASE("module scaffold links") {
    CHECK(f3d::api::module_name() == "api");
}
