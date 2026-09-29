#pragma once

#include "AYEditor/EditorAnimationDocument.h"

#include <AYUI/Widget.h>
#include <AYUI/Authoring/PreviewViewport.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ayt::editor {

class EditorAnimationCanvas final : public ayt::ui::Widget {
public:
    explicit EditorAnimationCanvas(
        std::shared_ptr<EditorAnimationDocument> document);
    ~EditorAnimationCanvas() override;
    void setControlHandle(std::string id) { _controlId=std::move(id); markDirty(); }
    void setOnControlSelected(std::function<void(const std::string&)> selected) { _onControlSelected=std::move(selected); }

    void framePreview();
    void setOnBoneSelected(std::function<void(int)> callback) {
        _onBoneSelected = std::move(callback);
    }

    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseWheel(const ayt::ui::UIMouseWheelEvent& event) override;
    void onMouseLeave() override;
    void onCaptureCancelled() override;
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
    void finishControlDrag(bool cancel);
    struct ControlPoint { std::string id; ayt::math::FVector3 world; ProjectedPoint projected; bool rotation=false; };
    std::vector<ControlPoint> _controls;
    std::optional<ayt::ui::authoring::PreviewProjection> _projection,_dragProjection;
    std::string _controlId;
    bool _controlDragging=false;
    ayt::math::FVector2 _dragPointer;
    ProjectedPoint _dragCenter;
    ayt::anim::editor::RigControlPose _dragPose;
    std::vector<ayt::math::Float4x4> _dragWorld;
    std::function<void(const std::string&)> _onControlSelected;

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
