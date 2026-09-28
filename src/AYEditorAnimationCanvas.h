#pragma once

#include "AYEditor/EditorAnimationDocument.h"

#include <AYUI/Widget.h>
#include <AYUI/Authoring/PreviewViewport.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ayt::editor {

class EditorAnimationCanvas final : public ayt::ui::Widget {
public:
    explicit EditorAnimationCanvas(
        std::shared_ptr<EditorAnimationDocument> document);

    void framePreview();
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
    struct WorldSegment {
        ayt::math::FVector3 a{};
        ayt::math::FVector3 b{};
    };
    struct ProjectedSegment {
        ProjectedPoint a{};
        ProjectedPoint b{};
    };

    void rebuildProjection();
    void rebuildModelSegments();
    int hitBone(ayt::math::FVector2 point) const noexcept;

    std::shared_ptr<EditorAnimationDocument> _document;
    std::vector<ayt::math::FVector3> _skeletonWorld;
    std::vector<ProjectedPoint> _skeletonProjected;
    std::vector<WorldSegment> _modelWorldSegments;
    std::vector<ProjectedSegment> _modelProjectedSegments;
    ayt::ui::authoring::PreviewOrbit _orbit;
    ayt::ui::authoring::PreviewProjectionCache _projectionCache;
    std::function<void(int)> _onBoneSelected;
};

} // namespace ayt::editor
