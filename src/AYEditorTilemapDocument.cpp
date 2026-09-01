#include "AYEditor/EditorTilemapDocument.h"

#include <AYIO/File.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <limits>
#include <queue>
#include <utility>

namespace ayt::editor
{

namespace
{

void setError(std::string* error, const std::string& message)
{
    if (error != nullptr) *error = message;
}

std::string documentName(const std::string& path)
{
    std::string name = std::filesystem::path(path).filename().string();
    for (int i = 0; i < 2; ++i) {
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos) break;
        name.resize(dot);
    }
    return name;
}

} // namespace

size_t EditorTilemapDocument::indexOf(uint32_t col, uint32_t row) const noexcept
{
    return static_cast<size_t>(row) * _cols + col;
}

bool EditorTilemapDocument::create(uint32_t cols, uint32_t rows,
                                   uint32_t tileWidth, uint32_t tileHeight,
                                   uint32_t defaultTileId)
{
    const uint64_t count = static_cast<uint64_t>(cols) * rows;
    if (cols == 0u || rows == 0u || tileWidth == 0u || tileHeight == 0u
        || count > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    _path.clear();
    _cols = cols;
    _rows = rows;
    _tileWidth = tileWidth;
    _tileHeight = tileHeight;
    _defaultTileId = defaultTileId;
    _tiles.assign(static_cast<size_t>(count), defaultTileId);
    _collisionFlags.clear();
    _animations.clear();
    _dirty = true;
    return true;
}

uint32_t EditorTilemapDocument::tileAt(uint32_t col, uint32_t row) const noexcept
{
    if (col >= _cols || row >= _rows) return _defaultTileId;
    return _tiles[indexOf(col, row)];
}

bool EditorTilemapDocument::paint(uint32_t col, uint32_t row,
                                  uint32_t tileId) noexcept
{
    if (col >= _cols || row >= _rows) return false;
    uint32_t& current = _tiles[indexOf(col, row)];
    if (current == tileId) return true;
    current = tileId;
    _dirty = true;
    return true;
}

uint32_t EditorTilemapDocument::floodFill(uint32_t col, uint32_t row,
                                          uint32_t tileId)
{
    if (col >= _cols || row >= _rows) return 0u;
    const uint32_t replaced = tileAt(col, row);
    if (replaced == tileId) return 0u;
    std::queue<std::pair<uint32_t, uint32_t>> pending;
    pending.push({col, row});
    uint32_t changed = 0u;
    while (!pending.empty()) {
        const auto [x, y] = pending.front();
        pending.pop();
        if (x >= _cols || y >= _rows || tileAt(x, y) != replaced) continue;
        _tiles[indexOf(x, y)] = tileId;
        ++changed;
        if (x > 0u) pending.push({x - 1u, y});
        if (x + 1u < _cols) pending.push({x + 1u, y});
        if (y > 0u) pending.push({x, y - 1u});
        if (y + 1u < _rows) pending.push({x, y + 1u});
    }
    _dirty = _dirty || changed > 0u;
    return changed;
}

void EditorTilemapDocument::setCollisionFlags(uint32_t tileId, uint32_t flags)
{
    if (flags == 0u) _collisionFlags.erase(tileId);
    else _collisionFlags[tileId] = flags;
    _dirty = true;
}

void EditorTilemapDocument::setAnimation(
    uint32_t sourceTileId, std::vector<EditorTileAnimationFrame> frames)
{
    if (frames.empty()) _animations.erase(sourceTileId);
    else _animations[sourceTileId] = std::move(frames);
    _dirty = true;
}

bool EditorTilemapDocument::load(const std::string& path, std::string* error)
{
    ayt::io::File file(path, ayt::io::File::Mode::Read);
    if (!file.isOpen()) {
        setError(error, "Could not open tilemap document.");
        return false;
    }
    std::string source(file.size(), '\0');
    if (!source.empty() && file.read(source.data(), source.size()) != source.size()) {
        setError(error, "Could not read tilemap document.");
        return false;
    }
    try {
        const nlohmann::json root = nlohmann::json::parse(source);
        const uint32_t cols = root.at("cols").get<uint32_t>();
        const uint32_t rows = root.at("rows").get<uint32_t>();
        const uint32_t tileWidth = root.at("tileWidth").get<uint32_t>();
        const uint32_t tileHeight = root.at("tileHeight").get<uint32_t>();
        const uint32_t defaultTileId = root.value("defaultTileId", 0u);
        EditorTilemapDocument loaded;
        if (!loaded.create(cols, rows, tileWidth, tileHeight, defaultTileId)) {
            setError(error, "Tilemap dimensions are invalid.");
            return false;
        }
        if (root.contains("tiles")) {
            const auto& tiles = root.at("tiles");
            if (!tiles.is_array() || tiles.size() > loaded._tiles.size()) {
                setError(error, "Tile array exceeds the declared grid.");
                return false;
            }
            for (size_t i = 0; i < tiles.size(); ++i) {
                loaded._tiles[i] = tiles[i].get<uint32_t>();
            }
        }
        for (const auto& entry : root.value("collisionFlags", nlohmann::json::array())) {
            loaded._collisionFlags[entry.at("tileId").get<uint32_t>()]
                = entry.at("flags").get<uint32_t>();
        }
        for (const auto& entry : root.value("animations", nlohmann::json::array())) {
            std::vector<EditorTileAnimationFrame> frames;
            for (const auto& frame : entry.at("frames")) {
                frames.push_back({frame.at("tileId").get<uint32_t>(),
                                  frame.at("durationMs").get<uint32_t>()});
            }
            if (!frames.empty()) {
                loaded._animations[entry.at("sourceTileId").get<uint32_t>()]
                    = std::move(frames);
            }
        }
        loaded._path = path;
        loaded._dirty = false;
        *this = std::move(loaded);
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& ex) {
        setError(error, ex.what());
        return false;
    }
}

bool EditorTilemapDocument::save(const std::string& path, std::string* error)
{
    const std::string destination = path.empty() ? _path : path;
    if (destination.empty() || _cols == 0u || _rows == 0u) {
        setError(error, "Tilemap document has no destination or grid.");
        return false;
    }
    nlohmann::json root;
    root["name"] = documentName(destination);
    root["cols"] = _cols;
    root["rows"] = _rows;
    root["tileWidth"] = _tileWidth;
    root["tileHeight"] = _tileHeight;
    root["defaultTileId"] = _defaultTileId;
    root["mode"] = "wide32";
    root["tiles"] = _tiles;
    root["collisionFlags"] = nlohmann::json::array();
    for (const auto& [tileId, flags] : _collisionFlags) {
        root["collisionFlags"].push_back({{"tileId", tileId}, {"flags", flags}});
    }
    root["animations"] = nlohmann::json::array();
    for (const auto& [sourceTileId, frames] : _animations) {
        nlohmann::json entry;
        entry["sourceTileId"] = sourceTileId;
        entry["frames"] = nlohmann::json::array();
        for (const EditorTileAnimationFrame& frame : frames) {
            entry["frames"].push_back(
                {{"tileId", frame.tileId}, {"durationMs", frame.durationMs}});
        }
        root["animations"].push_back(std::move(entry));
    }
    const std::string serialized = root.dump(2) + "\n";
    if (!ayt::io::File::atomicWrite(destination, serialized.data(),
                                    serialized.size())) {
        setError(error, "Atomic tilemap save failed.");
        return false;
    }
    _path = destination;
    _dirty = false;
    if (error != nullptr) error->clear();
    return true;
}

} // namespace ayt::editor
