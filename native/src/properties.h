#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rn {

// Builds /dev/__properties__ from the sysroot's build.prop files, Refract's defaults and
// `overrides` (which win).
void SetupSystemProperties(const std::wstring& sysroot, const std::wstring& data_dir,
                           const std::vector<std::pair<std::string, std::string>>& overrides);

}  // namespace rn
