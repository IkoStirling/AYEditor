#pragma once
// AYInspectorOverrides.h - Phase 1 ED-03.
//
// User-edited overrides that the Inspector applies to the live
// selected character entity's SkeletonComponent / AnimationComponent
// paths. Lives outside AYEditorPlayRuntime.h because both
// EditorSession (UI-driven) and EditorPlayRuntime (runtime-driven)
// need to reference the struct. Plain-data; follows the same
// pattern as ImportedCharacter in AYEditorPlayRuntime.h and
// ImportedCharacterMapDiagnostics in AYImportedCharacterMapper.h.

#include <string>

namespace ayt::editor
{

struct EntityInspectorOverrides
{
    // Empty = no override; the ImportedCharacter-derived path on
    // the spawned entity is left intact. Both fields carry the
    // same shape as ImportedCharacter paths (cache-rooted absolute
    // or virtual relative), so the AnimationComponent / Skeleton
    // Component can store them verbatim and let resolveAssetPath
    // resolve at runtime.
    std::string skeletonPathOverride;  // empty => leave SkeletonComponent::skeletonPath
    std::string animationPathOverride; // empty => leave AnimationComponent::clipPath

    void clear()
    {
        skeletonPathOverride.clear();
        animationPathOverride.clear();
    }

    bool isCleared() const
    {
        return skeletonPathOverride.empty() && animationPathOverride.empty();
    }
};

} // namespace ayt::editor