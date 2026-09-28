#pragma once

#include "AYEditor/EditorAnimationDocument.h"

#include <AYUI/Widget.h>

#include <functional>
#include <memory>
#include <string>

namespace ayt::editor {

class EditorAnimationDopeSheet final : public ayt::ui::Widget {
public:
    explicit EditorAnimationDopeSheet(
        std::shared_ptr<EditorAnimationDocument> document);

    void frameAll();
    void setSelection(std::string trackId, std::string keyId);
    void setOnSelectionChanged(
        std::function<void(const std::string&, const std::string&)> callback) {
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
    struct KeyHit {
        std::string trackId;
        std::string keyId;
    };

    ayt::math::FRectangle plotBounds() const noexcept;
    double secondsAt(float x) const noexcept;
    float worldX(double seconds) const noexcept;
    KeyHit hitKey(ayt::math::FVector2 point) const;
    void finishDrag(bool cancel);

    std::shared_ptr<EditorAnimationDocument> _document;
    std::string _selectedTrackId;
    std::string _selectedKeyId;
    std::string _dragKeyId;
    double _viewStart = 0.0;
    double _viewDuration = 1.0;
    bool _viewValid = false;
    bool _panning = false;
    bool _draggingKey = false;
    bool _gestureChanged = false;
    ayt::math::FVector2 _lastPointer{};
    std::function<void(const std::string&, const std::string&)>
        _onSelectionChanged;
    std::function<void()> _onEdited;
};

} // namespace ayt::editor
