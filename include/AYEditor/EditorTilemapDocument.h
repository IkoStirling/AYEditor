#pragma once

#include <AY2DEditor/TilemapDocument.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor
{

using EditorTileAnimationFrame = ayt::ay2d::editor::TileAnimationFrame;

// Authoring model for AYResource's `.aytilemap.json` schema. It deliberately
// contains no AYUI widget or GPU handle and is safe to use in import tools.
class EditorTilemapDocument {
public:
    bool create(uint32_t cols, uint32_t rows,
                uint32_t tileWidth, uint32_t tileHeight,
                uint32_t defaultTileId = 0u);
    bool load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path = {}, std::string* error = nullptr);

    [[nodiscard]] uint32_t cols() const noexcept { return _document.cols(); }
    [[nodiscard]] uint32_t rows() const noexcept { return _document.rows(); }
    [[nodiscard]] uint32_t tileWidth() const noexcept { return _document.tileWidth(); }
    [[nodiscard]] uint32_t tileHeight() const noexcept { return _document.tileHeight(); }
    [[nodiscard]] bool dirty() const noexcept { return _document.dirty(); }
    [[nodiscard]] const std::string& path() const noexcept { return _document.path(); }

    [[nodiscard]] uint32_t tileAt(uint32_t col, uint32_t row) const noexcept;
    bool paint(uint32_t col, uint32_t row, uint32_t tileId) noexcept;
    uint32_t floodFill(uint32_t col, uint32_t row, uint32_t tileId);
    void setCollisionFlags(uint32_t tileId, uint32_t flags);
    void setAnimation(uint32_t sourceTileId,
                      std::vector<EditorTileAnimationFrame> frames);

    [[nodiscard]] const std::map<uint32_t, uint32_t>& collisionFlags() const noexcept {
        return _document.collisionFlags();
    }
    [[nodiscard]] const std::map<uint32_t,
        std::vector<EditorTileAnimationFrame>>& animations() const noexcept {
        return _document.animations();
    }

private:
    // Compatibility facade over the single AY2D authoring model. New editor
    // features should consume AY2DEditor::TilemapDocument directly.
    ayt::ay2d::editor::TilemapDocument _document;
};

} // namespace ayt::editor
