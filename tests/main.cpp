// SPDX-License-Identifier: Apache-2.0
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "f3d/core/system.hpp"

int main(int argc, char** argv) {
    f3d::core::disable_error_dialogs(); // a failed assert must fail the test, not wait for a click
    doctest::Context context(argc, argv);
    return context.run();
}
