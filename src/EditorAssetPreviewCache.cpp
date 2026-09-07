#include "EditorAssetPreviewCache.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <utility>

namespace ayt::editor {

namespace {

std::string lowerExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return extension;
}

} // namespace

EditorAssetPreviewCache::EditorAssetPreviewCache(
    CreateTexture createTexture, ReleaseTexture releaseTexture)
    : _createTexture(std::move(createTexture)),
      _releaseTexture(std::move(releaseTexture))
{
}

EditorAssetPreviewCache::~EditorAssetPreviewCache()
{
    clear();
}

bool EditorAssetPreviewCache::supports(const EditorAssetRecord& record)
{
    if (record.type != EditorAssetType::Texture) return false;
    const std::string extension = lowerExtension(record.absolutePath);
    return extension == ".png" || extension == ".jpg"
        || extension == ".jpeg" || extension == ".bmp"
        || extension == ".tga";
}

EditorAssetPreviewCache::DecodedImage EditorAssetPreviewCache::decode(
    const std::string& path)
{
    DecodedImage result;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int components = 0;
    stbi_uc* rgba = stbi_load(
        path.c_str(), &sourceWidth, &sourceHeight, &components, 4);
    if (rgba == nullptr || sourceWidth <= 0 || sourceHeight <= 0) {
        if (rgba != nullptr) stbi_image_free(rgba);
        return result;
    }

    constexpr int maxPreviewEdge = 512;
    const float scale = std::min(
        1.0f, static_cast<float>(maxPreviewEdge)
            / static_cast<float>(std::max(sourceWidth, sourceHeight)));
    result.width = std::max(1, static_cast<int>(
        std::round(static_cast<float>(sourceWidth) * scale)));
    result.height = std::max(1, static_cast<int>(
        std::round(static_cast<float>(sourceHeight) * scale)));
    result.bgra.resize(static_cast<std::size_t>(result.width)
                       * static_cast<std::size_t>(result.height) * 4u);

    for (int y = 0; y < result.height; ++y) {
        const int sourceY = std::min(
            sourceHeight - 1, y * sourceHeight / result.height);
        for (int x = 0; x < result.width; ++x) {
            const int sourceX = std::min(
                sourceWidth - 1, x * sourceWidth / result.width);
            const std::size_t sourceIndex =
                (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX)
                * 4u;
            const std::size_t targetIndex =
                (static_cast<std::size_t>(y) * result.width + x) * 4u;
            result.bgra[targetIndex + 0u] = rgba[sourceIndex + 2u];
            result.bgra[targetIndex + 1u] = rgba[sourceIndex + 1u];
            result.bgra[targetIndex + 2u] = rgba[sourceIndex + 0u];
            result.bgra[targetIndex + 3u] = rgba[sourceIndex + 3u];
        }
    }
    stbi_image_free(rgba);
    return result;
}

ayt::ui::ImageTextureHandle EditorAssetPreviewCache::request(
    const EditorAssetRecord& record)
{
    if (!_createTexture || !supports(record) || record.absolutePath.empty()) {
        return {};
    }

    auto found = _entries.find(record.absolutePath);
    if (found != _entries.end()
        && (found->second.fileSize != record.size
            || found->second.lastModified != record.lastModified)) {
        release(found->second);
        _entries.erase(found);
        found = _entries.end();
    }
    if (found != _entries.end()) return found->second.texture;

    Entry entry;
    entry.fileSize = record.size;
    entry.lastModified = record.lastModified;
    entry.pending = true;
    const std::string path = record.absolutePath;
    entry.future = std::async(std::launch::async,
        [path]() { return decode(path); });
    _entries.emplace(path, std::move(entry));
    return {};
}

bool EditorAssetPreviewCache::poll()
{
    bool changed = false;
    for (auto& pair : _entries) {
        Entry& entry = pair.second;
        if (entry.pending && entry.future.valid()
            && entry.future.wait_for(std::chrono::seconds(0))
                == std::future_status::ready) {
            entry.decoded = entry.future.get();
            entry.pending = false;
            entry.failed = entry.decoded.bgra.empty();
        }
        if (!entry.pending && !entry.failed && !entry.texture.isValid()
            && !entry.decoded.bgra.empty()) {
            void* handle = _createTexture(
                static_cast<std::uint16_t>(entry.decoded.width),
                static_cast<std::uint16_t>(entry.decoded.height),
                entry.decoded.bgra.data());
            if (handle != nullptr) {
                entry.texture.handle = handle;
                entry.texture.width = entry.decoded.width;
                entry.texture.height = entry.decoded.height;
                entry.texture.format = ayt::ui::TextureFormat::RGBA8;
                entry.decoded.bgra.clear();
                entry.decoded.bgra.shrink_to_fit();
                changed = true;
            }
        }
    }
    return changed;
}

void EditorAssetPreviewCache::release(Entry& entry)
{
    if (entry.texture.isValid() && _releaseTexture) {
        _releaseTexture(entry.texture.handle);
    }
    entry.texture = {};
}

void EditorAssetPreviewCache::erase(const std::string& absolutePath)
{
    const auto found = _entries.find(absolutePath);
    if (found == _entries.end()) return;
    release(found->second);
    _entries.erase(found);
}

void EditorAssetPreviewCache::clear()
{
    for (auto& pair : _entries) release(pair.second);
    _entries.clear();
}

} // namespace ayt::editor
