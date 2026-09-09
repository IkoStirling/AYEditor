#pragma once

#include "AYEditor/EditorAssetDatabase.h"

#include <string>
#include <vector>

namespace ayt::editor {

struct EditorAssetReference {
    EditorAssetId targetId = 0;
    std::string targetPath;
    EditorAssetId referencingId = 0;
    std::string referencingPath;
};

struct EditorAssetDeleteAnalysis {
    std::vector<EditorAssetReference> references;

    bool hasExternalReferences() const noexcept { return !references.empty(); }
};

// Examines cached database records and reads only small text authoring files.
// It never loads runtime resources, so a delete confirmation cannot trigger a
// mesh/material import or stall on binary decoding.
EditorAssetDeleteAnalysis analyzeEditorAssetDeletion(
    const EditorAssetDatabase& database,
    const std::vector<EditorAssetId>& selectedIds);

} // namespace ayt::editor
