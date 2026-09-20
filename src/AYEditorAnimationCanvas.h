#pragma once

#include "AYEditor/EditorAnimationDocument.h"

#include <AYUI/Widget.h>

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
    ayt::ui::UiCursorHint getCursorHint() const override;

protected:
    void onRender(ayt::ui::IRenderBackend& renderer) override;

private:
    struct ProjectedPoint {
        float x = 0.0f;
        float y = 0.0f;
        float depth = 0.0f;
    };
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
    ayt::math::FRectangle _projectedBounds{};
    std::uint64_t _projectedRevision = 0u;
    std::uint64_t _projectedPoseRevision = 0u;
    float _projectedYaw = 0.0f;
    float _projectedPitch = 0.0f;
    float _projectedZoom = 0.0f;
    bool _projectionValid = false;
    float _yaw = 0.55f;
    float _pitch = -0.18f;
    float _zoom = 1.0f;
    bool _rotating = false;
    ayt::math::FVector2 _lastPointer{};
    std::function<void(int)> _onBoneSelected;
};

} // namespace ayt::editor
