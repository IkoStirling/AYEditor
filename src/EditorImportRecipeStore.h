#pragma once

#include <AYResource/ImportJob.h>
#include <string>
#include <vector>

namespace ayt::editor::detail {

using StoredImportRecipe = ayt::resource::ProjectImportRecipe;

// Author data lives outside .ayeditor_cache and .ayeditor/cache. Relative
// references in the JSON are resolved against the current project directory.
std::vector<StoredImportRecipe> loadImportRecipes(const std::string& projectRoot,
                                                 std::string* error = nullptr);
bool sameImportSource(const std::string& first, const std::string& second);
bool saveImportRecipe(const std::string& projectRoot,
                      ayt::resource::ImportOptions& options,
                      std::string* error = nullptr);

} // namespace ayt::editor::detail
