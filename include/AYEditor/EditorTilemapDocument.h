#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ayt::editor
{

struct EditorTileAnimationFrame {
    uint32_t tileId = 0;
    uint32_t durationMs = 0;
};

// Authoring model for AYResource's `.aytilemap.json` schema. It deliberately
// contains no AYUI widget or GPU handle and is safe to use in import tools.
class EditorTilemapDocument {
public:
    bool create(uint32_t cols, uint32_t rows,
                uint32_t tileWidth, uint32_t tileHeight,
                uint32_t defaultTileId = 0u);
    bool load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path = {}, std::string* error = nullptr);

    [[nodiscard]] uint32_t cols() const noexcept { return _cols; }
    [[nodiscard]] uint32_t rows() const noexcept { return _rows; }
    [[nodiscard]] uint32_t tileWidth() const noexcept { return _tileWidth; }
    [[nodiscard]] uint32_t tileHeight() const noexcept { return _tileHeight; }
    [[nodiscard]] bool dirty() const noexcept { return _dirty; }
    [[nodiscard]] const std::string& path() const noexcept { return _path; }

    [[nodiscard]] uint32_t tileAt(uint32_t col, uint32_t row) const noexcept;
    bool paint(uint32_t col, uint32_t row, uint32_t tileId) noexcept;
    uint32_t floodFill(uint32_t col, uint32_t row, uint32_t tileId);
    void setCollisionFlags(uint32_t tileId, uint32_t flags);
    void setAnimation(uint32_t sourceTileId,
                      std::vector<EditorTileAnimationFrame> frames);

    [[nodiscard]] const std::map<uint32_t, uint32_t>& collisionFlags() const noexcept {
        return _collisionFlags;
    }
    [[nodiscard]] const std::map<uint32_t,
        std::vector<EditorTileAnimationFrame>>& animations() const noexcept {
        return _animations;
    }

private:
    [[nodiscard]] size_t indexOf(uint32_t col, uint32_t row) const noexcept;

    std::string _path;
    uint32_t _cols = 0;
    uint32_t _rows = 0;
    uint32_t _tileWidth = 0;
    uint32_t _tileHeight = 0;
    uint32_t _defaultTileId = 0;
    std::vector<uint32_t> _tiles;
    std::map<uint32_t, uint32_t> _collisionFlags;
    std::map<uint32_t, std::vector<EditorTileAnimationFrame>> _animations;
    bool _dirty = false;
};

} // namespace ayt::editor
