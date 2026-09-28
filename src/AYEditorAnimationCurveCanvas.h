#pragma once

#include "AYEditor/EditorAnimationDocument.h"

#include <AYUI/Widget.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace ayt::editor {

class EditorAnimationCurveCanvas final : public ayt::ui::Widget {
public:
    explicit EditorAnimationCurveCanvas(
        std::shared_ptr<EditorAnimationDocument> document);

    void setTrackId(std::string trackId);
    const std::string& trackId() const noexcept { return _trackId; }
    void setComponentVisible(std::size_t component, bool visible);
    bool componentVisible(std::size_t component) const noexcept;
    void frameAll();
    void setOnSelectionChanged(
        std::function<void(const std::string&, std::size_t)> callback) {
        _onSelectionChanged = std::move(callback);
    }
    void setOnEdited(std::function<void()> callback) {
        _onEdited = std::move(callback);
    }

    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseWheel(const ayt::ui::UIMouseWheelEvent& event) override;
    void onCaptureCancelled() override;
    ayt::ui::UiCursorHint getCursorHint() const override;

protected:
    void onRender(ayt::ui::IRenderBackend& renderer) override;

private:
    enum class HitKind { None, Key, InTangent, OutTangent };
    struct Hit {
        HitKind kind = HitKind::None;
        std::string keyId;
        std::size_t component = 0u;
    };

    ayt::math::FRectangle plotBounds() const noexcept;
    bool loadTrack(EditorAnimationCurveTrack& track) const;
    void ensureView(const EditorAnimationCurveTrack& track);
    float worldX(double seconds) const noexcept;
    float worldY(float value) const noexcept;
    double secondsAt(float x) const noexcept;
    float valueAt(float y) const noexcept;
    Hit hitTest(ayt::math::FVector2 point,
                const EditorAnimationCurveTrack& track) const;
    void finishGesture(bool cancel);

    std::shared_ptr<EditorAnimationDocument> _document;
    std::string _trackId;
    std::string _selectedKeyId;
    std::array<bool, 4> _componentVisible{{true, true, true, true}};
    std::function<void(const std::string&, std::size_t)> _onSelectionChanged;
    std::function<void()> _onEdited;
    double _viewStart = 0.0;
    double _viewDuration = 1.0;
    float _valueCenter = 0.0f;
    float _valueSpan = 2.0f;
    bool _viewValid = false;
    bool _panning = false;
    bool _gestureChanged = false;
    Hit _dragHit;
    ayt::math::FVector2 _lastPointer{};
};

} // namespace ayt::editor
