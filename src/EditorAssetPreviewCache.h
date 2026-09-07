#pragma once

#include "AYEditor/EditorAssetDatabase.h"
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
    bool poll();
    void erase(const std::string& absolutePath);
    void clear();
    std::size_t size() const noexcept { return _entries.size(); }

private:
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> bgra;
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

    static bool supports(const EditorAssetRecord& record);
    static DecodedImage decode(const std::string& path);
    void release(Entry& entry);

    CreateTexture _createTexture;
    ReleaseTexture _releaseTexture;
    std::unordered_map<std::string, Entry> _entries;
};

} // namespace ayt::editor
