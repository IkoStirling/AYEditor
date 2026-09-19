#pragma once

#include "AYEditor/EditorSkeletonDocument.h"

#include <AYUI/Widget.h>

#include <functional>
#include <memory>
#include <vector>

namespace ayt::editor {

class EditorSkeletonCanvas final : public ayt::ui::Widget {
public:
    explicit EditorSkeletonCanvas(
        std::shared_ptr<EditorSkeletonDocument> document);

    void frameSkeleton();
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
    struct ProjectedPoint { float x = 0.0f; float y = 0.0f; float depth = 0.0f; };
    void rebuildProjection();
    int hitBone(ayt::math::FVector2 worldPoint) const noexcept;

    std::shared_ptr<EditorSkeletonDocument> _document;
    std::vector<ProjectedPoint> _projected;
    float _yaw = 0.55f;
    float _pitch = -0.18f;
    float _zoom = 1.0f;
    bool _rotating = false;
    ayt::math::FVector2 _lastPointer{};
    std::function<void(int)> _onBoneSelected;
};

} // namespace ayt::editor
