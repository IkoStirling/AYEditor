#pragma once

#include <AY2DEditor/SpriteAnimationAuthoringModel.h>
#include <AYUI/ImageTexture.h>
#include <AYUI/Widget.h>

#include <cstdint>
#include <functional>

namespace ayt::editor {

class EditorSpriteAnimationCanvas final : public ayt::ui::Widget {
public:
    explicit EditorSpriteAnimationCanvas(bool previewOnly = false);

    void setModel(
        ayt::ay2d::editor::SpriteAnimationAuthoringModel* model) noexcept;
    void setImage(ayt::ui::ImageTextureHandle texture,
                  uint32_t width, uint32_t height);
    void clearImage();
    void frameImage();
    void setOnSelectionChanged(std::function<void()> callback) {
        _onSelectionChanged = std::move(callback);
    }

    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseWheel(const ayt::ui::UIMouseWheelEvent& event) override;
    void onMouseLeave() override;
    ayt::ui::UiCursorHint getCursorHint() const override;
    void tick(float dt) override;

protected:
    void onRender(ayt::ui::IRenderBackend& renderer) override;

private:
    [[nodiscard]] ayt::math::FRectangle imageRect() const noexcept;
    [[nodiscard]] ayt::math::FRectangle cellRect(
        uint32_t cell) const noexcept;
    [[nodiscard]] bool sourcePoint(ayt::math::FVector2 point,
                                   float& x, float& y,
                                   bool clamp) const noexcept;
    [[nodiscard]] uint32_t hitCell(ayt::math::FVector2 point) const noexcept;
    void renderSheet(ayt::ui::IRenderBackend& renderer,
                     const ayt::math::FRectangle& bounds);
    void renderPreview(ayt::ui::IRenderBackend& renderer,
                       const ayt::math::FRectangle& bounds);

    ayt::ay2d::editor::SpriteAnimationAuthoringModel* _model = nullptr;
    ayt::ui::ImageTextureHandle _texture;
    uint32_t _imageWidth = 0u;
    uint32_t _imageHeight = 0u;
    float _zoom = 1.0f;
    ayt::math::FVector2 _pan{0.0f, 0.0f};
    ayt::math::FVector2 _lastPointer{0.0f, 0.0f};
    uint32_t _selectionAnchor = 0u;
    uint32_t _hoverCell = 0u;
    bool _previewOnly = false;
    bool _selecting = false;
    bool _panning = false;
    bool _hoverValid = false;
    std::function<void()> _onSelectionChanged;
};

} // namespace ayt::editor
