#include "AYEditor/EditorTilemapDocument.h"

#include <AYResource/Converter/TilemapConverter.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace ayt::editor {

bool EditorTilemapDocument::create(uint32_t cols, uint32_t rows,
                                   uint32_t tileWidth, uint32_t tileHeight,
                                   uint32_t defaultTileId)
{
    return _document.create(cols, rows, tileWidth, tileHeight, defaultTileId);
}

bool EditorTilemapDocument::load(const std::string& path, std::string* error)
{
    return _document.load(path, error);
}

bool EditorTilemapDocument::save(const std::string& path, std::string* error)
{
    const std::string destination = path.empty() ? _document.path() : path;
    if (!_document.save(destination, error)) return false;
    const std::filesystem::path source =
        std::filesystem::absolute(destination).lexically_normal();
    std::filesystem::path assetRoot;
    for (std::filesystem::path cursor = source.parent_path();
         !cursor.empty(); cursor = cursor.parent_path()) {
        if (cursor.filename() == "Assets") {
            assetRoot = cursor;
            break;
        }
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) break;
    }
    if (assetRoot.empty()) {
        // Standalone documents and tests may intentionally live outside a
        // project. The authoring save remains valid there; automatic cooking
        // is a project-Assets convention.
        if (error != nullptr) error->clear();
        return true;
    }
    ayt::resource::TilemapConverter converter(source.string());
    converter.setOutputDir(assetRoot.string());
    const ayt::resource::ConversionResult cooked = converter.convert();
    const size_t tilemapCount = static_cast<size_t>(std::count_if(
        cooked.resources.begin(), cooked.resources.end(),
        [](const ayt::resource::ConversionResult::ConvertedResource& resource) {
            return resource.type == "Tilemap";
        }));
    if (tilemapCount != 1u) {
        if (error != nullptr) {
            *error = "Tilemap source was saved, but runtime cooking failed.";
        }
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

uint32_t EditorTilemapDocument::tileAt(
    uint32_t col, uint32_t row) const noexcept
{
    return _document.tileAt(col, row);
}

bool EditorTilemapDocument::paint(
    uint32_t col, uint32_t row, uint32_t tileId) noexcept
{
    return _document.paint(col, row, tileId);
}

uint32_t EditorTilemapDocument::floodFill(
    uint32_t col, uint32_t row, uint32_t tileId)
{
    return _document.floodFill(col, row, tileId);
}

void EditorTilemapDocument::setCollisionFlags(
    uint32_t tileId, uint32_t flags)
{
    (void)_document.setCollisionFlags(tileId, flags);
}

void EditorTilemapDocument::setAnimation(
    uint32_t sourceTileId, std::vector<EditorTileAnimationFrame> frames)
{
    (void)_document.setAnimation(sourceTileId, std::move(frames));
}

} // namespace ayt::editor
