#pragma once

#include <AY2DEditor/TilemapDocument.h>
#include <AYUI/ImageTexture.h>
#include <AYUI/Widget.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ayt::editor {

// AYEditor presentation of an atlas in source space. Transparent/skipped
// cells remain visible in their original grid positions.
class EditorTileAtlasPicker final : public ayt::ui::Widget {
public:
    struct Cell {
        uint32_t tileId = 0u;
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 0u;
        uint32_t sourceHeight = 0u;
        bool operator==(const Cell&) const = default;
    };
    struct SourceRect {
        uint32_t x = 0u;
        uint32_t y = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        bool operator==(const SourceRect&) const = default;
    };

    void setAtlas(const ayt::ui::ImageTextureHandle& texture,
                  const ayt::ay2d::editor::TileAtlasSource& source,
                  std::vector<Cell> cells);
    void clearAtlas();
    void setSelectedTileId(uint32_t tileId);
    void frameAtlas();
    void setRectangleSelectionEnabled(bool enabled,
                                      bool snapToGrid = false);
    [[nodiscard]] bool rectangleSelectionEnabled() const noexcept {
        return _rectangleSelectionEnabled;
    }
    void clearRectangleSelection();
    void setOnTileSelected(std::function<void(uint32_t)> callback) {
        _onTileSelected = std::move(callback);
    }
    void setOnStatus(std::function<void(const std::wstring&)> callback) {
        _onStatus = std::move(callback);
    }
    void setOnRectangleSelected(
        std::function<void(const SourceRect&)> callback) {
        _onRectangleSelected = std::move(callback);
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
    struct GridHit {
        int column = -1;
        int row = -1;
        int cellIndex = -1;
        const Cell* cell = nullptr;
        [[nodiscard]] bool valid() const noexcept {
            return (column >= 0 && row >= 0) || cellIndex >= 0;
        }
    };
    [[nodiscard]] ayt::math::FRectangle imageRect() const noexcept;
    [[nodiscard]] ayt::math::FRectangle cellRect(int column,
                                                  int row) const noexcept;
    [[nodiscard]] ayt::math::FRectangle cellRect(
        const Cell& cell) const noexcept;
    [[nodiscard]] std::optional<ayt::math::FVector2> sourcePoint(
        ayt::math::FVector2 point, bool clampToImage) const noexcept;
    [[nodiscard]] SourceRect selectionFromPoints(
        ayt::math::FVector2 first,
        ayt::math::FVector2 last) const noexcept;
    [[nodiscard]] GridHit hit(ayt::math::FVector2 point) const noexcept;
    [[nodiscard]] const Cell* importedCell(uint32_t sourceX,
                                           uint32_t sourceY) const noexcept;
    void updateHover(ayt::math::FVector2 point);

    ayt::ui::ImageTextureHandle _texture;
    ayt::ay2d::editor::TileAtlasSource _source;
    std::vector<Cell> _cells;
    uint32_t _selectedTileId = 0u;
    float _zoom = 1.0f;
    ayt::math::FVector2 _pan{0.0f, 0.0f};
    ayt::math::FVector2 _lastPointer{0.0f, 0.0f};
    bool _panning = false;
    bool _rectangleSelectionEnabled = false;
    bool _selectionSnapsToGrid = false;
    bool _selectingRectangle = false;
    ayt::math::FVector2 _selectionStart{0.0f, 0.0f};
    std::optional<SourceRect> _selection;
    int _hoverColumn = -1;
    int _hoverRow = -1;
    int _hoverCellIndex = -1;
    std::function<void(uint32_t)> _onTileSelected;
    std::function<void(const std::wstring&)> _onStatus;
    std::function<void(const SourceRect&)> _onRectangleSelected;
};

} // namespace ayt::editor
