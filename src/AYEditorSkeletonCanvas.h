#pragma once

#include "AYEditor/EditorSkeletonDocument.h"

#include <AYUI/Widget.h>
#include <AYUI/Authoring/PreviewViewport.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ayt::editor {

class EditorSkeletonCanvas final : public ayt::ui::Widget {
public:
    explicit EditorSkeletonCanvas(
        std::shared_ptr<EditorSkeletonDocument> document);

    void frameSkeleton();
    [[nodiscard]] bool hasSideBySidePreview() const noexcept {
        return !_targetProjected.empty();
    }
    void setOnBoneSelected(std::function<void(int)> callback) {
        _onBoneSelected = std::move(callback);
    }

    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseWheel(const ayt::ui::UIMouseWheelEvent& event) override;
    void onMouseLeave() override;
    void onCaptureCancelled() override { _orbit.cancel(); }
    ayt::ui::UiCursorHint getCursorHint() const override;

protected:
    void onRender(ayt::ui::IRenderBackend& renderer) override;

private:
    using ProjectedPoint = ayt::ui::authoring::PreviewPoint;
    void rebuildProjection();
    int hitBone(ayt::math::FVector2 worldPoint) const noexcept;

    std::shared_ptr<EditorSkeletonDocument> _document;
    std::vector<ayt::math::FVector3> _worldPoints;
    std::vector<ProjectedPoint> _projected;
    std::vector<ayt::math::FVector3> _targetWorldPoints;
    std::vector<ProjectedPoint> _targetProjected;
    ayt::ui::authoring::PreviewOrbit _orbit;
    ayt::ui::authoring::PreviewProjectionCache _projectionCache;
    std::function<void(int)> _onBoneSelected;
};

} // namespace ayt::editor
