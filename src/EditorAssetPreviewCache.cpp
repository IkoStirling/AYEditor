#include "EditorAssetPreviewCache.h"

#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYResource/assetsImpl/Mesh.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
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

std::uint64_t stableHash(const std::string& value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
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

void EditorAssetPreviewCache::setDiskCacheRoot(std::string rootPath)
{
    _diskCacheRoot = std::move(rootPath);
    if (!_diskCacheRoot.empty()) {
        std::error_code ignored;
        std::filesystem::create_directories(_diskCacheRoot, ignored);
    }
}

std::string EditorAssetPreviewCache::cachePathFor(
    const std::string& absolutePath) const
{
    if (_diskCacheRoot.empty()) return {};
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0')
         << stableHash(absolutePath) << ".aypreview";
    return (std::filesystem::path(_diskCacheRoot) / name.str()).string();
}

EditorAssetPreviewCache::DecodedImage
EditorAssetPreviewCache::loadDiskPreview(
    const std::string& cachePath, std::uintmax_t sourceSize,
    std::int64_t sourceModified)
{
    DecodedImage image;
    if (cachePath.empty()) return image;
    std::ifstream input(cachePath, std::ios::binary);
    char magic[8]{};
    std::uint32_t version = 0;
    std::uint64_t storedSize = 0;
    std::int64_t storedModified = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint64_t byteCount = 0;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&version), sizeof(version));
    input.read(reinterpret_cast<char*>(&storedSize), sizeof(storedSize));
    input.read(reinterpret_cast<char*>(&storedModified), sizeof(storedModified));
    input.read(reinterpret_cast<char*>(&width), sizeof(width));
    input.read(reinterpret_cast<char*>(&height), sizeof(height));
    input.read(reinterpret_cast<char*>(&byteCount), sizeof(byteCount));
    const std::uint64_t expected = width > 0 && height > 0
        ? static_cast<std::uint64_t>(width) * height * 4u : 0u;
    if (!input || std::memcmp(magic, "AYPREV1", 7) != 0 || version != 1u
        || storedSize != sourceSize || storedModified != sourceModified
        || byteCount != expected || expected > 64u * 1024u * 1024u) {
        return {};
    }
    image.width = width;
    image.height = height;
    image.bgra.resize(static_cast<std::size_t>(byteCount));
    input.read(reinterpret_cast<char*>(image.bgra.data()),
               static_cast<std::streamsize>(image.bgra.size()));
    if (!input) return {};
    image.fromDisk = true;
    return image;
}

void EditorAssetPreviewCache::storeDiskPreview(
    const std::string& cachePath, std::uintmax_t sourceSize,
    std::int64_t sourceModified, const DecodedImage& image)
{
    if (cachePath.empty() || image.bgra.empty()) return;
    std::error_code error;
    const std::filesystem::path path(cachePath);
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const char magic[8] = {'A','Y','P','R','E','V','1','\0'};
    const std::uint32_t version = 1u;
    const std::uint64_t storedSize = sourceSize;
    const std::int64_t storedModified = sourceModified;
    const std::int32_t width = image.width;
    const std::int32_t height = image.height;
    const std::uint64_t bytes = image.bgra.size();
    output.write(magic, sizeof(magic));
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&storedSize), sizeof(storedSize));
    output.write(reinterpret_cast<const char*>(&storedModified), sizeof(storedModified));
    output.write(reinterpret_cast<const char*>(&width), sizeof(width));
    output.write(reinterpret_cast<const char*>(&height), sizeof(height));
    output.write(reinterpret_cast<const char*>(&bytes), sizeof(bytes));
    output.write(reinterpret_cast<const char*>(image.bgra.data()),
                 static_cast<std::streamsize>(image.bgra.size()));
    output.close();
    if (!output) return;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}

bool EditorAssetPreviewCache::supports(const EditorAssetRecord& record)
{
    if (record.type == EditorAssetType::Texture) {
        const std::string extension = lowerExtension(record.absolutePath);
        return extension == ".png" || extension == ".jpg"
            || extension == ".jpeg" || extension == ".bmp"
            || extension == ".tga";
    }
    return record.type == EditorAssetType::Mesh
        || record.type == EditorAssetType::SourceModel
        || record.type == EditorAssetType::Material
        || record.type == EditorAssetType::Animation
        || record.type == EditorAssetType::Skeleton;
}

EditorAssetPreviewCache::DecodedImage
EditorAssetPreviewCache::renderResourcePreview(
    const std::string& path, EditorAssetType type)
{
    DecodedImage result;
    result.width = 144;
    result.height = 96;
    result.bgra.assign(static_cast<std::size_t>(result.width * result.height * 4),
                       0u);
    auto pixel = [&result](int x, int y, std::uint8_t r, std::uint8_t g,
                           std::uint8_t b, std::uint8_t a = 255u) {
        if (x < 0 || y < 0 || x >= result.width || y >= result.height) return;
        const std::size_t index = static_cast<std::size_t>(
            (y * result.width + x) * 4);
        result.bgra[index + 0u] = b;
        result.bgra[index + 1u] = g;
        result.bgra[index + 2u] = r;
        result.bgra[index + 3u] = a;
    };
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            const std::uint8_t shade = static_cast<std::uint8_t>(23 + y / 8);
            pixel(x, y, shade, shade + 3u, shade + 9u);
        }
    }
    auto line = [&pixel](int x0, int y0, int x1, int y1,
                         std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            pixel(x0, y0, r, g, b);
            if (x0 == x1 && y0 == y1) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    };

    if (type == EditorAssetType::Material) {
        std::uint8_t surfaceR = 76u;
        std::uint8_t surfaceG = 158u;
        std::uint8_t surfaceB = 232u;
        ayt::resource::Material material;
        if (material.load(path)
            && material.hasParameter("baseColor")) {
            float color[4] = {0.30f, 0.62f, 0.91f, 1.0f};
            material.getFloat4("baseColor", color);
            surfaceR = static_cast<std::uint8_t>(255.0f
                * std::clamp(color[0], 0.0f, 1.0f));
            surfaceG = static_cast<std::uint8_t>(255.0f
                * std::clamp(color[1], 0.0f, 1.0f));
            surfaceB = static_cast<std::uint8_t>(255.0f
                * std::clamp(color[2], 0.0f, 1.0f));
        }
        for (int y = 14; y < 84; ++y) {
            for (int x = 37; x < 107; ++x) {
                const float nx = (x - 72.0f) / 35.0f;
                const float ny = (y - 49.0f) / 35.0f;
                const float radius = nx * nx + ny * ny;
                if (radius > 1.0f) continue;
                const float nz = std::sqrt(std::max(0.0f, 1.0f - radius));
                const float light = std::clamp(
                    0.22f + 0.78f * (-0.35f * nx - 0.45f * ny + 0.82f * nz),
                    0.12f, 1.0f);
                pixel(x, y, static_cast<std::uint8_t>(surfaceR * light),
                      static_cast<std::uint8_t>(surfaceG * light),
                      static_cast<std::uint8_t>(surfaceB * light));
            }
        }
    } else if (type == EditorAssetType::Animation) {
        line(18, 76, 126, 76, 82, 104, 132);
        line(18, 18, 18, 76, 82, 104, 132);
        ayt::resource::Animation animation;
        bool actualCurve = animation.load(path)
            && animation.getTrackCount() > 0u
            && animation.getTrackKeyframeCount(0u) > 1u;
        if (actualCurve) {
            const std::uint32_t count = animation.getTrackKeyframeCount(0u);
            const float* values = animation.getTrackValues(0u);
            const float* times = animation.getTrackTimes(0u);
            const int components = animation.getTrackType(0u)
                    == ayt::resource::AnimTrackType::Quaternion ? 4
                : animation.getTrackType(0u)
                    == ayt::resource::AnimTrackType::Vector3 ? 3 : 1;
            float minimum = (std::numeric_limits<float>::max)();
            float maximum = (std::numeric_limits<float>::lowest)();
            for (std::uint32_t index = 0; index < count; ++index) {
                minimum = std::min(minimum, values[index * components]);
                maximum = std::max(maximum, values[index * components]);
            }
            const float timeSpan = std::max(times[count - 1u] - times[0], 1.0e-5f);
            const float valueSpan = std::max(maximum - minimum, 1.0e-5f);
            int previousX = 18;
            int previousY = 66;
            for (std::uint32_t index = 0; index < count; ++index) {
                const int x = 18 + static_cast<int>(108.0f
                    * (times[index] - times[0]) / timeSpan);
                const int y = 72 - static_cast<int>(48.0f
                    * (values[index * components] - minimum) / valueSpan);
                if (index != 0u) line(previousX, previousY, x, y, 98, 183, 255);
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox)
                        pixel(x + ox, y + oy, 235, 179, 74);
                previousX = x;
                previousY = y;
            }
        } else {
            int previousX = 18;
            int previousY = 66;
            for (int x = 19; x <= 126; ++x) {
                const float t = static_cast<float>(x - 18) / 108.0f;
                const int y = static_cast<int>(64.0f - 35.0f
                    * (t * t * (3.0f - 2.0f * t))
                    + 7.0f * std::sin(t * 16.0f));
                line(previousX, previousY, x, y, 98, 183, 255);
                previousX = x;
                previousY = y;
            }
        }
    } else if (type == EditorAssetType::Skeleton) {
        const std::pair<int, int> joints[] = {
            {72, 15}, {72, 31}, {52, 45}, {92, 45}, {72, 55},
            {56, 80}, {88, 80}};
        line(72, 15, 72, 55, 122, 202, 255);
        line(72, 31, 52, 45, 122, 202, 255);
        line(72, 31, 92, 45, 122, 202, 255);
        line(72, 55, 56, 80, 122, 202, 255);
        line(72, 55, 88, 80, 122, 202, 255);
        for (const auto& joint : joints) {
            for (int oy = -2; oy <= 2; ++oy)
                for (int ox = -2; ox <= 2; ++ox)
                    pixel(joint.first + ox, joint.second + oy, 238, 189, 83);
        }
    } else {
        ayt::resource::Mesh mesh;
        const bool actualMesh = type == EditorAssetType::Mesh
            && mesh.load(path) && mesh.getVertexCount() > 0u
            && mesh.hasAttribute(ayt::resource::MeshAttribute::Position);
        if (actualMesh) {
            struct Point { float x; float y; };
            std::vector<Point> points(mesh.getVertexCount());
            float minX = (std::numeric_limits<float>::max)();
            float minY = minX;
            float maxX = (std::numeric_limits<float>::lowest)();
            float maxY = maxX;
            const auto positionInfo = mesh.getAttributeInfo(
                ayt::resource::MeshAttribute::Position);
            for (std::uint32_t index = 0; index < mesh.getVertexCount(); ++index) {
                float position[3]{};
                std::memcpy(position, mesh.getVertexData()
                    + static_cast<std::size_t>(index) * mesh.getVertexStride()
                    + positionInfo.offset, sizeof(position));
                // A fixed isometric projection produces stable thumbnails.
                points[index] = {position[0] + position[2] * 0.38f,
                                 position[1] - position[2] * 0.22f};
                minX = std::min(minX, points[index].x);
                maxX = std::max(maxX, points[index].x);
                minY = std::min(minY, points[index].y);
                maxY = std::max(maxY, points[index].y);
            }
            const float spanX = std::max(maxX - minX, 1.0e-5f);
            const float spanY = std::max(maxY - minY, 1.0e-5f);
            auto projected = [&](std::uint32_t index) {
                const Point& point = points[index];
                return std::pair<int, int>{
                    12 + static_cast<int>(120.0f * (point.x - minX) / spanX),
                    86 - static_cast<int>(76.0f * (point.y - minY) / spanY)};
            };
            const std::uint32_t* indices = mesh.getIndexData();
            const std::uint32_t triangleCount = std::min<std::uint32_t>(
                mesh.getIndexCount() / 3u, 1600u);
            const std::uint32_t step = std::max<std::uint32_t>(1u,
                (mesh.getIndexCount() / 3u) / std::max(1u, triangleCount));
            for (std::uint32_t triangle = 0;
                 triangle < mesh.getIndexCount() / 3u; triangle += step) {
                const std::uint32_t ia = indices[triangle * 3u];
                const std::uint32_t ib = indices[triangle * 3u + 1u];
                const std::uint32_t ic = indices[triangle * 3u + 2u];
                if (ia >= points.size() || ib >= points.size()
                    || ic >= points.size()) continue;
                const auto a = projected(ia);
                const auto b = projected(ib);
                const auto c = projected(ic);
                line(a.first, a.second, b.first, b.second, 99, 183, 255);
                line(b.first, b.second, c.first, c.second, 75, 145, 211);
                line(c.first, c.second, a.first, a.second, 75, 145, 211);
            }
        } else {
            const std::pair<int, int> points[] = {
                {42, 30}, {89, 22}, {111, 42}, {101, 76},
                {52, 82}, {29, 57}};
            for (int index = 0; index < 6; ++index) {
                const auto& a = points[index];
                const auto& b = points[(index + 1) % 6];
                line(a.first, a.second, b.first, b.second, 99, 183, 255);
                line(a.first, a.second, 72, 54, 69, 133, 193);
            }
            line(42, 30, 101, 76, 235, 179, 74);
            line(89, 22, 52, 82, 235, 179, 74);
        }
    }
    return result;
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
    const EditorAssetType type = record.type;
    const std::string cachePath = cachePathFor(path);
    const std::uintmax_t sourceSize = record.size;
    const std::int64_t sourceModified = record.lastModified;
    entry.future = std::async(std::launch::async,
        [path, type, cachePath, sourceSize, sourceModified]() {
        DecodedImage image = loadDiskPreview(
            cachePath, sourceSize, sourceModified);
        if (!image.bgra.empty()) return image;
        image = type == EditorAssetType::Texture
            ? decode(path) : renderResourcePreview(path, type);
        storeDiskPreview(cachePath, sourceSize, sourceModified, image);
        return image;
    });
    _entries.emplace(path, std::move(entry));
    return {};
}

EditorAuthoringImage EditorAssetPreviewCache::loadAuthoringImage(
    const std::string& path, std::string* error)
{
    if (error != nullptr) error->clear();
    if (!_createTexture || path.empty()) {
        if (error != nullptr) *error = "The image service is unavailable.";
        return {};
    }
    std::error_code fileError;
    const std::filesystem::path source =
        std::filesystem::absolute(std::filesystem::u8path(path), fileError)
            .lexically_normal();
    if (fileError || !std::filesystem::is_regular_file(source, fileError)) {
        if (error != nullptr) *error = "The source image does not exist.";
        return {};
    }
    const std::string key = source.string();
    const std::uintmax_t fileSize = std::filesystem::file_size(source, fileError);
    if (fileError) {
        if (error != nullptr) *error = "The source image cannot be inspected.";
        return {};
    }
    const auto modified = std::filesystem::last_write_time(source, fileError);
    const std::int64_t lastModified = fileError ? 0
        : static_cast<std::int64_t>(modified.time_since_epoch().count());
    auto found = _authoringEntries.find(key);
    if (found != _authoringEntries.end()
        && (found->second.fileSize != fileSize
            || found->second.lastModified != lastModified)) {
        release(found->second);
        _authoringEntries.erase(found);
        found = _authoringEntries.end();
    }
    if (found != _authoringEntries.end()) return found->second.image;

    int width = 0;
    int height = 0;
    int components = 0;
    stbi_uc* rgba = stbi_load(key.c_str(), &width, &height, &components, 4);
    if (rgba == nullptr || width <= 0 || height <= 0) {
        if (rgba != nullptr) stbi_image_free(rgba);
        if (error != nullptr) *error = "The source image could not be decoded.";
        return {};
    }
    const uint64_t pixelCount = static_cast<uint64_t>(width)
        * static_cast<uint64_t>(height);
    if (pixelCount > 64u * 1024u * 1024u
        || width > (std::numeric_limits<uint16_t>::max)()
        || height > (std::numeric_limits<uint16_t>::max)()) {
        stbi_image_free(rgba);
        if (error != nullptr) {
            *error = "The source image exceeds the authoring texture budget.";
        }
        return {};
    }
    auto pixels = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(pixelCount) * 4u);
    for (size_t pixel = 0u; pixel < static_cast<size_t>(pixelCount); ++pixel) {
        (*pixels)[pixel * 4u + 0u] = rgba[pixel * 4u + 2u];
        (*pixels)[pixel * 4u + 1u] = rgba[pixel * 4u + 1u];
        (*pixels)[pixel * 4u + 2u] = rgba[pixel * 4u + 0u];
        (*pixels)[pixel * 4u + 3u] = rgba[pixel * 4u + 3u];
    }
    stbi_image_free(rgba);
    void* handle = _createTexture(
        static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        pixels->data());
    if (handle == nullptr) {
        if (error != nullptr) *error = "The source texture could not be uploaded.";
        return {};
    }

    AuthoringEntry entry;
    entry.fileSize = fileSize;
    entry.lastModified = lastModified;
    entry.image.texture.handle = handle;
    entry.image.texture.width = width;
    entry.image.texture.height = height;
    entry.image.texture.format = ayt::ui::TextureFormat::RGBA8;
    entry.image.width = static_cast<uint32_t>(width);
    entry.image.height = static_cast<uint32_t>(height);
    entry.image.bgraPixels = std::move(pixels);
    const EditorAuthoringImage result = entry.image;
    _authoringEntries.emplace(key, std::move(entry));
    return result;
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
            if (entry.decoded.fromDisk) ++_diskCacheHits;
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

void EditorAssetPreviewCache::release(AuthoringEntry& entry)
{
    if (entry.image.texture.isValid() && _releaseTexture) {
        _releaseTexture(entry.image.texture.handle);
    }
    entry.image = {};
}

void EditorAssetPreviewCache::erase(const std::string& absolutePath)
{
    const auto found = _entries.find(absolutePath);
    if (found != _entries.end()) {
        release(found->second);
        _entries.erase(found);
    }
}

void EditorAssetPreviewCache::clear()
{
    for (auto& pair : _entries) release(pair.second);
    _entries.clear();
    for (auto& pair : _authoringEntries) release(pair.second);
    _authoringEntries.clear();
}

} // namespace ayt::editor
