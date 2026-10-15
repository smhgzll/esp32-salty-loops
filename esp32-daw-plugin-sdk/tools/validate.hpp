// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>

#include "daw_plugin_api.h"

struct ValidationResult {
    std::vector<std::string> failures, warnings;
};
ValidationResult validatePlugin(const daw_plugin_descriptor_t *d);
