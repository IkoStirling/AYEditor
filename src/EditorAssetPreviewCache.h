#pragma once

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/EditorExtension.h"
#include "AYUI/ImageTexture.h"

#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor {

// Decodes raster previews away from the UI thread and uploads each distinct
// (path, size, modification-time) revision once. GPU handles remain owned by
// this cache; virtualized TileCells only borrow them while visible.
class EditorAssetPreviewCache {
public:
    using CreateTexture =
        std::function<void*(std::uint16_t, std::uint16_t, const void*)>;
    using ReleaseTexture = std::function<void(void*)>;

    EditorAssetPreviewCache(CreateTexture createTexture,
                            ReleaseTexture releaseTexture);
    ~EditorAssetPreviewCache();

    EditorAssetPreviewCache(const EditorAssetPreviewCache&) = delete;
    EditorAssetPreviewCache& operator=(const EditorAssetPreviewCache&) = delete;

    ayt::ui::ImageTextureHandle request(const EditorAssetRecord& record);
    EditorAuthoringImage loadAuthoringImage(
        const std::string& path, std::string* error = nullptr);
    static bool supports(const EditorAssetRecord& record);
    void setDiskCacheRoot(std::string rootPath);
    const std::string& diskCacheRoot() const noexcept { return _diskCacheRoot; }
    std::size_t diskCacheHitCount() const noexcept { return _diskCacheHits; }
    bool poll();
    void erase(const std::string& absolutePath);
    void clear();
    std::size_t size() const noexcept { return _entries.size(); }

private:
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> bgra;
        bool fromDisk = false;
    };
    struct Entry {
        std::uintmax_t fileSize = 0;
        std::int64_t lastModified = 0;
        std::future<DecodedImage> future;
        DecodedImage decoded;
        ayt::ui::ImageTextureHandle texture;
        bool pending = false;
        bool failed = false;
    };
    struct AuthoringEntry {
        std::uintmax_t fileSize = 0;
        std::int64_t lastModified = 0;
        EditorAuthoringImage image;
    };

    static DecodedImage decode(const std::string& path);
    static DecodedImage renderResourcePreview(
        const std::string& path, EditorAssetType type);
    static DecodedImage loadDiskPreview(
        const std::string& cachePath, std::uintmax_t sourceSize,
        std::int64_t sourceModified);
    static void storeDiskPreview(
        const std::string& cachePath, std::uintmax_t sourceSize,
        std::int64_t sourceModified, const DecodedImage& image);
    std::string cachePathFor(const std::string& absolutePath) const;
    void release(Entry& entry);
    void release(AuthoringEntry& entry);

    CreateTexture _createTexture;
    ReleaseTexture _releaseTexture;
    std::string _diskCacheRoot;
    std::size_t _diskCacheHits = 0;
    std::unordered_map<std::string, Entry> _entries;
    std::unordered_map<std::string, AuthoringEntry> _authoringEntries;
};

} // namespace ayt::editor
