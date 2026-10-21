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
// Optimistic editor guard, not a cross-process compare-and-swap. A dialog
// opens from the latest persisted settings and refuses known stale Apply.
struct ImportRecipeRevision {
    std::string projectRoot;
    ayt::resource::ImportOptions lookup;
    std::string baseline;
    bool existed = false;
};
bool readImportRecipeForEdit(const std::string& projectRoot,
                            ayt::resource::ImportOptions& options,
                            ImportRecipeRevision& revision,
                            std::string* error = nullptr);
bool verifyImportRecipeRevision(const ImportRecipeRevision& revision,
                                std::string* error = nullptr);
bool saveImportRecipe(const std::string& projectRoot,
                      ayt::resource::ImportOptions& options,
                      std::string* error = nullptr);

} // namespace ayt::editor::detail
