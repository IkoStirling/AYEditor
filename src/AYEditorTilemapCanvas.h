#pragma once

#include <AY2DEditor/TilemapEditorModel.h>
#include <AY2DEditor/Viewport2DModel.h>
#include <AYUI/Widget.h>
#include <AYUI/ImageTexture.h>

#include <functional>
#include <map>

namespace ayt::editor {

// AYEditor presentation/input adapter for the renderer-independent AY2D
// authoring model. Atlas texture ownership intentionally remains outside this
// widget until the editor exposes a shared preview-texture service.
class EditorTilemapCanvas final : public ayt::ui::Widget {
public:
    explicit EditorTilemapCanvas(ayt::ay2d::editor::TilemapEditorModel& model);

    void setAtlasTextures(
        const std::map<uint32_t, ayt::ui::ImageTextureHandle>* textures) {
        _atlasTextures = textures;
        markDirty();
    }

    void frameDocument();
    void setShowGrid(bool show);
    [[nodiscard]] bool showGrid() const noexcept { return _showGrid; }
    void setShowCollision(bool show);
    [[nodiscard]] bool showCollision() const noexcept {
        return _showCollision;
    }
    void setShowShadows(bool show) { _showShadows = show; markDirty(); }
    [[nodiscard]] bool showShadows() const noexcept { return _showShadows; }
    void setSpacePan(bool enabled);
    [[nodiscard]] bool spacePan() const noexcept { return _spacePan; }
    [[nodiscard]] bool editingGestureActive() const noexcept;

    void setOnEdited(std::function<void()> callback) {
        _onEdited = std::move(callback);
    }
    void setOnTilePicked(std::function<void(uint32_t)> callback) {
        _onTilePicked = std::move(callback);
    }
    void setOnViewChanged(std::function<void(float)> callback) {
        _onViewChanged = std::move(callback);
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
    [[nodiscard]] ayt::math::FVector2 localPoint(
        ayt::math::FVector2 worldPoint) const;
    [[nodiscard]] ayt::ay2d::editor::TileCell cellAt(
        ayt::math::FVector2 worldPoint) const;
    [[nodiscard]] bool isInDocument(
        ayt::ay2d::editor::TileCell cell) const noexcept;
    [[nodiscard]] ayt::ay2d::editor::TileCell clampToDocument(
        ayt::ay2d::editor::TileCell cell) const noexcept;
    [[nodiscard]] uint32_t visibleTileAt(
        ayt::ay2d::editor::TileCell cell) const noexcept;
    void applyCell(ayt::ay2d::editor::TileCell cell);
    void applyStrokeSegment(ayt::ay2d::editor::TileCell from,
                            ayt::ay2d::editor::TileCell to);
    void updateHover(ayt::math::FVector2 worldPoint);
    void notifyViewChanged();
    [[nodiscard]] bool tileTextureVisual(
        uint32_t tileId, ayt::ui::ImageTextureHandle& texture,
        ayt::math::FRectangle& uv) const noexcept;

    ayt::ay2d::editor::TilemapEditorModel& _model;
    ayt::ay2d::editor::Viewport2DModel _viewport;
    const std::map<uint32_t, ayt::ui::ImageTextureHandle>*
        _atlasTextures = nullptr;
    bool _cameraInitialized = false;
    bool _showGrid = true;
    bool _showCollision = true;
    bool _showShadows = true;
    bool _drawing = false;
    bool _panning = false;
    bool _spacePan = false;
    int _panButton = -1;
    bool _rectangleDrawing = false;
    bool _selectionDrawing = false;
    bool _selectionMoving = false;
    ayt::ay2d::editor::TileCell _hover{-1, -1};
    ayt::ay2d::editor::TileCell _lastPainted{-1, -1};
    ayt::ay2d::editor::TileCell _rectangleStart{-1, -1};
    ayt::ay2d::editor::TileCell _rectangleCurrent{-1, -1};
    ayt::ay2d::editor::TileCell _selectionStart{-1, -1};
    ayt::ay2d::editor::TileCell _selectionCurrent{-1, -1};
    ayt::math::FVector2 _lastPointer{0.0f, 0.0f};
    ayt::math::FVector2 _lastCanvasSize{0.0f, 0.0f};
    std::function<void()> _onEdited;
    std::function<void(uint32_t)> _onTilePicked;
    std::function<void(float)> _onViewChanged;
};

} // namespace ayt::editor
