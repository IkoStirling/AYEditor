#include "AYEditor/EditorAssetDatabase.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <system_error>

namespace ayt::editor {

namespace {

std::string slashNormalized(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool startsWithFolder(const std::string& path, const std::string& folder)
{
    if (path == folder) return true;
    return path.size() > folder.size()
        && path.compare(0, folder.size(), folder) == 0
        && path[folder.size()] == '/';
}

std::string stripRootPrefix(const std::string& path,
                            const std::string& root)
{
    if (path.empty() || root.empty()) return {};
    const std::string foldedPath = lowerAscii(path);
    const std::string foldedRoot = lowerAscii(root);
    if (foldedPath == foldedRoot) return {};
    if (foldedPath.size() <= foldedRoot.size()
        || foldedPath.compare(0, foldedRoot.size(), foldedRoot) != 0
        || path[foldedRoot.size()] != '/') {
        return {};
    }
    return path.substr(foldedRoot.size() + 1);
}

std::string stripBrowserRoot(std::string path)
{
    const std::string folded = lowerAscii(path);
    constexpr std::size_t assetsPrefixLength = 7;
    constexpr std::size_t importedPrefixLength = 9;
    if (folded.rfind("assets/", 0) == 0) {
        return path.substr(assetsPrefixLength);
    }
    if (folded.rfind("imported/", 0) == 0) {
        return path.substr(importedPrefixLength);
    }
    return path;
}

std::string logicalParent(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::string logicalName(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

EditorAssetId stableAssetId(const std::string& logicalPath)
{
    // FNV-1a over the normalized, case-folded browser path. IDs remain stable
    // across rescans and process restarts without introducing sidecar files.
    constexpr EditorAssetId offset = 14695981039346656037ull;
    constexpr EditorAssetId prime = 1099511628211ull;
    EditorAssetId value = offset;
    for (unsigned char c : lowerAscii(slashNormalized(logicalPath))) {
        value ^= static_cast<EditorAssetId>(c);
        value *= prime;
    }
    return value == 0 ? 1 : value;
}

bool isMetadataSidecar(const std::filesystem::path& path)
{
    const std::string name = lowerAscii(path.filename().string());
    return name.size() >= 11
        && name.compare(name.size() - 11, 11, ".aydep.json") == 0;
}

template<typename SnapshotT>
void appendRoot(SnapshotT& snapshot,
                const std::filesystem::path& root,
                const char* virtualRoot,
                EditorAssetOrigin origin)
{
    snapshot.folders.push_back(EditorAssetFolder{
        virtualRoot, virtualRoot, {}, origin});

    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::directory_entry& entry = *it;
        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), root, ec);
        if (ec) break;
        const std::string relativeText = slashNormalized(relative.string());
        if (relativeText.empty() || relativeText == ".") continue;
        const std::string logical =
            std::string(virtualRoot) + "/" + relativeText;

        if (entry.is_directory(ec)) {
            snapshot.folders.push_back(EditorAssetFolder{
                logical, entry.path().filename().string(),
                logicalParent(logical), origin});
            continue;
        }
        if (!entry.is_regular_file(ec) || isMetadataSidecar(entry.path())) {
            continue;
        }

        EditorAssetRecord record;
        record.id = stableAssetId(logical);
        record.name = entry.path().filename().string();
        record.logicalPath = logical;
        record.absolutePath = slashNormalized(
            std::filesystem::absolute(entry.path(), ec).lexically_normal().string());
        if (ec) break;
        record.runtimePath = record.absolutePath;
        record.type = classifyEditorAssetPath(record.name);
        record.origin = origin;
        record.size = entry.file_size(ec);
        if (ec) {
            ec.clear();
            record.size = 0;
        }
        const auto modified = entry.last_write_time(ec);
        if (!ec) {
            record.lastModified = static_cast<std::int64_t>(
                modified.time_since_epoch().count());
        } else {
            ec.clear();
        }
        snapshot.records.push_back(std::move(record));
    }
    if (ec && snapshot.error.empty()) {
        snapshot.error = "asset scan failed under " + root.string()
            + ": " + ec.message();
    }
}

} // namespace

const char* editorAssetTypeName(EditorAssetType type) noexcept
{
    switch (type) {
    case EditorAssetType::Mesh: return "Mesh";
    case EditorAssetType::Material: return "Material";
    case EditorAssetType::Texture: return "Texture";
    case EditorAssetType::Scene: return "Scene";
    case EditorAssetType::Animation: return "Animation";
    case EditorAssetType::Skeleton: return "Skeleton";
    case EditorAssetType::Script: return "Script";
    case EditorAssetType::Shader: return "Shader";
    case EditorAssetType::Audio: return "Audio";
    case EditorAssetType::UiLayout: return "UI Layout";
    case EditorAssetType::SourceModel: return "Source Model";
    case EditorAssetType::Unknown: break;
    }
    return "File";
}

EditorAssetType classifyEditorAssetPath(const std::string& path)
{
    const std::string lower = lowerAscii(slashNormalized(path));
    if (lower.size() >= 8
        && lower.compare(lower.size() - 8, 8, ".ui.json") == 0) {
        return EditorAssetType::UiLayout;
    }
    const std::string extension =
        lowerAscii(std::filesystem::path(lower).extension().string());
    if (extension == ".aymesh") return EditorAssetType::Mesh;
    if (extension == ".aymat") return EditorAssetType::Material;
    if (extension == ".aytex" || extension == ".png"
        || extension == ".jpg" || extension == ".jpeg"
        || extension == ".tga" || extension == ".bmp"
        || extension == ".dds" || extension == ".hdr") {
        return EditorAssetType::Texture;
    }
    if (extension == ".ayscene") return EditorAssetType::Scene;
    if (extension == ".ayanm" || extension == ".ayanim") {
        return EditorAssetType::Animation;
    }
    if (extension == ".ayskel") return EditorAssetType::Skeleton;
    if (extension == ".lua" || extension == ".logia"
        || extension == ".py") return EditorAssetType::Script;
    if (extension == ".phoskia" || extension == ".shader" || extension == ".sc"
        || extension == ".vert" || extension == ".frag") {
        return EditorAssetType::Shader;
    }
    if (extension == ".wav" || extension == ".ogg"
        || extension == ".mp3" || extension == ".flac") {
        return EditorAssetType::Audio;
    }
    if (extension == ".fbx" || extension == ".gltf"
        || extension == ".glb" || extension == ".obj") {
        return EditorAssetType::SourceModel;
    }
    return EditorAssetType::Unknown;
}

EditorAssetDatabase::~EditorAssetDatabase()
{
    close();
}

bool EditorAssetDatabase::open(const std::string& projectRoot,
                               std::string* error)
{
    close();
    std::error_code ec;
    std::filesystem::path root = projectRoot.empty()
        ? std::filesystem::current_path(ec)
        : std::filesystem::absolute(projectRoot, ec);
    if (ec) {
        _lastError = "invalid project root: " + ec.message();
        if (error) *error = _lastError;
        return false;
    }
    root = root.lexically_normal();
    const std::filesystem::path source = root / "Assets";
    const std::filesystem::path derived = root / ".ayeditor_cache" / "assets";
    std::filesystem::create_directories(source, ec);
    if (!ec) std::filesystem::create_directories(derived, ec);
    if (ec) {
        _lastError = "cannot create project asset directories: " + ec.message();
        if (error) *error = _lastError;
        return false;
    }
    _projectRoot = slashNormalized(root.string());
    _sourceRoot = slashNormalized(source.string());
    _derivedRoot = slashNormalized(derived.string());
    Snapshot initial;
    initial.folders.push_back(EditorAssetFolder{
        "Assets", "Assets", {}, EditorAssetOrigin::Source});
    initial.folders.push_back(EditorAssetFolder{
        "Imported", "Imported", {}, EditorAssetOrigin::Imported});
    applySnapshot(std::move(initial));
    if (error) error->clear();
    return true;
}

void EditorAssetDatabase::close()
{
    if (_scanPending && _scanFuture.valid()) {
        try { (void)_scanFuture.get(); } catch (...) {}
    }
    _scanPending = false;
    _projectRoot.clear();
    _sourceRoot.clear();
    _derivedRoot.clear();
    _lastError.clear();
    _records.clear();
    _folders.clear();
    _recordById.clear();
    _recordByLogicalPath.clear();
}

EditorAssetDatabase::Snapshot EditorAssetDatabase::scanRoots(
    const std::filesystem::path& sourceRoot,
    const std::filesystem::path& derivedRoot)
{
    Snapshot snapshot;
    appendRoot(snapshot, sourceRoot, "Assets", EditorAssetOrigin::Source);
    appendRoot(snapshot, derivedRoot, "Imported", EditorAssetOrigin::Imported);
    auto byPath = [](const auto& a, const auto& b) {
        return lowerAscii(a.logicalPath) < lowerAscii(b.logicalPath);
    };
    std::sort(snapshot.folders.begin(), snapshot.folders.end(), byPath);
    snapshot.folders.erase(
        std::unique(snapshot.folders.begin(), snapshot.folders.end(),
            [](const EditorAssetFolder& a, const EditorAssetFolder& b) {
                return lowerAscii(a.logicalPath) == lowerAscii(b.logicalPath);
            }), snapshot.folders.end());
    std::sort(snapshot.records.begin(), snapshot.records.end(), byPath);
    return snapshot;
}

void EditorAssetDatabase::applySnapshot(Snapshot snapshot)
{
    _records = std::move(snapshot.records);
    _folders = std::move(snapshot.folders);
    _lastError = std::move(snapshot.error);
    _recordById.clear();
    _recordByLogicalPath.clear();
    for (std::size_t i = 0; i < _records.size(); ++i) {
        // A 64-bit hash collision is still handled deterministically: the
        // later record is assigned a second FNV stream by hashing its path
        // with an explicit suffix.
        std::size_t collision = 0;
        while (_recordById.find(_records[i].id) != _recordById.end()) {
            _records[i].id = stableAssetId(
                _records[i].logicalPath + "#" + std::to_string(i)
                + ":" + std::to_string(++collision));
        }
        _recordById.emplace(_records[i].id, i);
        _recordByLogicalPath.emplace(
            lowerAscii(_records[i].logicalPath), i);
    }
}

bool EditorAssetDatabase::scanNow(std::string* error)
{
    if (_projectRoot.empty()) {
        _lastError = "asset database is not open";
        if (error) *error = _lastError;
        return false;
    }
    if (_scanPending && _scanFuture.valid()) {
        try { (void)_scanFuture.get(); } catch (...) {}
        _scanPending = false;
    }
    applySnapshot(scanRoots(_sourceRoot, _derivedRoot));
    if (error) *error = _lastError;
    return _lastError.empty();
}

bool EditorAssetDatabase::requestScan()
{
    if (_projectRoot.empty() || _scanPending) return false;
    const std::filesystem::path source(_sourceRoot);
    const std::filesystem::path derived(_derivedRoot);
    _scanFuture = std::async(std::launch::async,
        [source, derived]() { return scanRoots(source, derived); });
    _scanPending = true;
    return true;
}

bool EditorAssetDatabase::pollScan()
{
    if (!_scanPending || !_scanFuture.valid()) return false;
    if (_scanFuture.wait_for(std::chrono::seconds(0))
        != std::future_status::ready) return false;
    try {
        applySnapshot(_scanFuture.get());
    } catch (const std::exception& e) {
        _lastError = std::string("asset scan failed: ") + e.what();
    } catch (...) {
        _lastError = "asset scan failed with an unknown error";
    }
    _scanPending = false;
    return true;
}

const EditorAssetRecord* EditorAssetDatabase::find(EditorAssetId id) const noexcept
{
    const auto it = _recordById.find(id);
    return it == _recordById.end() ? nullptr : &_records[it->second];
}

const EditorAssetRecord* EditorAssetDatabase::findByLogicalPath(
    const std::string& logicalPath) const noexcept
{
    const auto it = _recordByLogicalPath.find(
        lowerAscii(slashNormalized(logicalPath)));
    return it == _recordByLogicalPath.end() ? nullptr : &_records[it->second];
}

std::string EditorAssetDatabase::portableAssetPath(
    const EditorAssetRecord& record) const
{
    return stripBrowserRoot(slashNormalized(record.logicalPath));
}

std::string EditorAssetDatabase::portableAssetPath(
    const std::string& path) const
{
    const std::string normalized = slashNormalized(path);
    if (normalized.empty()) return {};
    if (const std::string relative = stripRootPrefix(normalized, _sourceRoot);
        !relative.empty()) {
        return relative;
    }
    if (const std::string relative = stripRootPrefix(normalized, _derivedRoot);
        !relative.empty()) {
        return relative;
    }
    return stripBrowserRoot(normalized);
}

std::vector<EditorAssetEntry> EditorAssetDatabase::entries(
    const std::string& folderPath,
    const std::string& query,
    std::optional<EditorAssetType> type) const
{
    const std::string folder = slashNormalized(folderPath);
    const std::string needle = lowerAscii(query);
    std::vector<EditorAssetEntry> result;
    if (needle.empty()) {
        for (const EditorAssetFolder& candidate : _folders) {
            if (candidate.parentPath == folder) {
                result.push_back(EditorAssetEntry{
                    true, candidate.logicalPath, 0, candidate.displayName,
                    EditorAssetType::Unknown, candidate.origin});
            }
        }
    }
    for (const EditorAssetRecord& record : _records) {
        if (type && record.type != *type) continue;
        const bool underFolder = startsWithFolder(record.logicalPath, folder);
        if (!underFolder) continue;
        if (needle.empty()) {
            if (logicalParent(record.logicalPath) != folder) continue;
        } else {
            const std::string haystack = lowerAscii(
                record.name + " " + record.logicalPath);
            if (haystack.find(needle) == std::string::npos) continue;
        }
        result.push_back(EditorAssetEntry{
            false, {}, record.id, record.name, record.type, record.origin});
    }
    std::sort(result.begin(), result.end(),
        [](const EditorAssetEntry& a, const EditorAssetEntry& b) {
            if (a.folder != b.folder) return a.folder > b.folder;
            return lowerAscii(a.displayName) < lowerAscii(b.displayName);
        });
    return result;
}

} // namespace ayt::editor
