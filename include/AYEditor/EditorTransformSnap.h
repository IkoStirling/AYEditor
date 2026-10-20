#pragma once
#include <cmath>

namespace ayt::editor {
/// Editor drag increments, measured from the drag's initial pose. Translation
/// follows the selected world/local handle axes; scale is a relative ratio.
/// Does not quantize the Scene, camera, or unselected transform components.
struct EditorTransformSnap {
    bool enabled = false;
    float translation = .5f;
    float rotationDegrees = 15.f;
    float scale = .1f;
    bool valid() const noexcept {
        return std::isfinite(translation) && translation >= 1e-5f && translation <= 10000.f
            && std::isfinite(rotationDegrees) && rotationDegrees >= .01f && rotationDegrees <= 180.f
            && std::isfinite(scale) && scale >= .001f && scale <= 10.f;
    }
    bool operator==(const EditorTransformSnap&) const = default;
};
} // namespace ayt::editor
