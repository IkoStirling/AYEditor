#include "AYEditor/EditorAssetDatabase.h"

#include <AYIO/FileWatcher.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <unordered_set>
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
                EditorAssetOrigin origin,
                const std::filesystem::path& importSidecarRoot = {})
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
        if (origin == EditorAssetOrigin::Imported) {
            record.importState = EditorAssetImportState::Ready;
        } else if (record.type == EditorAssetType::SourceModel) {
            record.importState = EditorAssetImportState::NeedsImport;
            const std::filesystem::path sidecar = importSidecarRoot
                / (entry.path().stem().string() + ".aydep.json");
            const auto sidecarSize = std::filesystem::file_size(sidecar, ec);
            if (!ec) {
                if (sidecarSize == 0u) {
                    record.importState = EditorAssetImportState::Failed;
                } else {
                    const auto sidecarModified =
                        std::filesystem::last_write_time(sidecar, ec);
                    if (!ec && sidecarModified >= modified) {
                        record.importState = EditorAssetImportState::Ready;
                    }
                }
            }
            ec.clear();
        }
        snapshot.records.push_back(std::move(record));
    }
    if (ec && snapshot.error.empty()) {
        snapshot.error = "asset scan failed under " + root.string()
            + ": " + ec.message();
    }
}

void writeIndex(const std::filesystem::path& path,
                const std::vector<EditorAssetRecord>& records)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "AYEDITOR_ASSET_INDEX\t1\n";
    for (const EditorAssetRecord& record : records) {
        output << record.id << '\t' << std::quoted(record.logicalPath) << '\t'
               << static_cast<unsigned>(record.type) << '\t'
               << static_cast<unsigned>(record.origin) << '\t'
               << static_cast<unsigned>(record.importState) << '\t'
               << record.size << '\t' << record.lastModified << '\n';
    }
    output.close();
    if (!output) return;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}

bool safeRelativeLogicalPath(const std::string& logicalPath,
                             const char* virtualRoot,
                             std::filesystem::path& relative)
{
    const std::string normalized = slashNormalized(logicalPath);
    const std::string prefix = std::string(virtualRoot) + "/";
    if (lowerAscii(normalized).rfind(lowerAscii(prefix), 0) != 0) return false;
    relative = std::filesystem::path(normalized.substr(prefix.size()))
        .lexically_normal();
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
}

template<typename SnapshotT>
void appendFolderChain(SnapshotT& snapshot,
                       const std::string& logicalPath,
                       EditorAssetOrigin origin)
{
    std::string folder = logicalParent(logicalPath);
    std::vector<std::string> chain;
    while (!folder.empty() && folder != "Assets" && folder != "Imported") {
        chain.push_back(folder);
        folder = logicalParent(folder);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        snapshot.folders.push_back(EditorAssetFolder{
            *it, logicalName(*it), logicalParent(*it), origin});
    }
}

template<typename SnapshotT>
bool readIndex(const std::filesystem::path& path,
               const std::filesystem::path& sourceRoot,
               const std::filesystem::path& derivedRoot,
               SnapshotT& snapshot)
{
    std::ifstream input(path, std::ios::binary);
    std::string marker;
    if (!std::getline(input, marker) || marker != "AYEDITOR_ASSET_INDEX\t1") {
        return false;
    }
    snapshot.folders.push_back(EditorAssetFolder{
        "Assets", "Assets", {}, EditorAssetOrigin::Source});
    snapshot.folders.push_back(EditorAssetFolder{
        "Imported", "Imported", {}, EditorAssetOrigin::Imported});
    EditorAssetRecord record;
    unsigned type = 0;
    unsigned origin = 0;
    unsigned importState = 0;
    while (input >> record.id >> std::quoted(record.logicalPath)
                 >> type >> origin >> importState
                 >> record.size >> record.lastModified) {
        if (type > static_cast<unsigned>(EditorAssetType::GameFlow)
            || origin > static_cast<unsigned>(EditorAssetOrigin::Imported)
            || importState > static_cast<unsigned>(EditorAssetImportState::Failed)) {
            return false;
        }
        record.type = static_cast<EditorAssetType>(type);
        record.origin = static_cast<EditorAssetOrigin>(origin);
        record.importState = static_cast<EditorAssetImportState>(importState);
        std::filesystem::path relative;
        const char* virtualRoot = record.origin == EditorAssetOrigin::Source
            ? "Assets" : "Imported";
        if (!safeRelativeLogicalPath(record.logicalPath, virtualRoot, relative)) {
            return false;
        }
        const std::filesystem::path absolute =
            (record.origin == EditorAssetOrigin::Source
                ? sourceRoot : derivedRoot) / relative;
        record.absolutePath = slashNormalized(
            absolute.lexically_normal().string());
        record.runtimePath = record.absolutePath;
        record.name = logicalName(record.logicalPath);
        appendFolderChain(snapshot, record.logicalPath, record.origin);
        snapshot.records.push_back(record);
    }
    if (!input.eof()) return false;
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
    return true;
}

} // namespace

struct EditorAssetDatabase::WatchState {
    ayt::io::FileWatcher watcher;
    std::unordered_set<std::string> directories;
};

EditorAssetDatabase::EditorAssetDatabase() = default;

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
    case EditorAssetType::Tilemap: return "Tilemap";
    case EditorAssetType::UiFlow: return "UI Flow";
    case EditorAssetType::GameFlow: return "Game Flow";
    case EditorAssetType::Unknown: break;
    }
    return "File";
}

const char* editorAssetImportStateName(EditorAssetImportState state) noexcept
{
    switch (state) {
    case EditorAssetImportState::NeedsImport: return "Needs import";
    case EditorAssetImportState::Ready: return "Ready";
    case EditorAssetImportState::Failed: return "Failed";
    case EditorAssetImportState::NotApplicable: break;
    }
    return "";
}

EditorAssetType classifyEditorAssetPath(const std::string& path)
{
    const std::string lower = lowerAscii(slashNormalized(path));
    if (lower.size() >= 14
        && lower.compare(lower.size() - 14, 14, ".gameflow.json") == 0) {
        return EditorAssetType::GameFlow;
    }
    if (lower.size() >= 12
        && lower.compare(lower.size() - 12, 12, ".uiflow.json") == 0) {
        return EditorAssetType::UiFlow;
    }
    if (lower.size() >= 8
        && lower.compare(lower.size() - 8, 8, ".ui.json") == 0) {
        return EditorAssetType::UiLayout;
    }
    if (lower.size() >= 15
        && lower.compare(lower.size() - 15, 15, ".aytilemap.json") == 0) {
        return EditorAssetType::Tilemap;
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
    if (extension == ".aytilemap") return EditorAssetType::Tilemap;
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
    if (extension == ".ayaudio" || extension == ".wav" || extension == ".ogg"
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
    _indexPath = slashNormalized((root / ".ayeditor" / "cache"
        / "asset-index.tsv").string());
    Snapshot initial;
    _loadedFromIndex = readIndex(_indexPath, source, derived, initial);
    if (!_loadedFromIndex) {
        initial.folders.push_back(EditorAssetFolder{
            "Assets", "Assets", {}, EditorAssetOrigin::Source});
        initial.folders.push_back(EditorAssetFolder{
            "Imported", "Imported", {}, EditorAssetOrigin::Imported});
    }
    applySnapshot(std::move(initial));
    _watchState = std::make_unique<WatchState>();
    refreshDirectoryWatches();
    _watchState->watcher.start();
    if (error) error->clear();
    return true;
}

void EditorAssetDatabase::close()
{
    if (_watchState) {
        // Stop the directory watcher first so it cannot enqueue new
        // requests against _records / _folders while we tear them down.
        _watchState->watcher.stop();
        _watchState.reset();
    }
    if (_scanPending && _scanFuture.valid()) {
        // B-3/B-4 (ayeditor audit 2026-09-14): bound the join so a slow
        // scan over a huge project tree cannot stretch the visible
        // shutdown path. After the budget expires we drop the future;
        // the worker keeps running in the background and its result is
        // discarded at process exit. (std::async cannot be forcibly
        // aborted — the std::future destructor will still block — but
        // only after the editor UI is gone.)
        constexpr auto kShutdownDrainBudget = std::chrono::milliseconds(250);
        if (_scanFuture.wait_for(kShutdownDrainBudget)
                != std::future_status::ready) {
            _scanFuture = {};
        } else {
            try { (void)_scanFuture.get(); } catch (...) {}
        }
    }
    _scanPending = false;
    _projectRoot.clear();
    _sourceRoot.clear();
    _derivedRoot.clear();
    _lastError.clear();
    _indexPath.clear();
    _records.clear();
    _folders.clear();
    _recordById.clear();
    _recordByLogicalPath.clear();
    _loadedFromIndex = false;
}

EditorAssetDatabase::Snapshot EditorAssetDatabase::scanRoots(
    const std::filesystem::path& sourceRoot,
    const std::filesystem::path& derivedRoot)
{
    Snapshot snapshot;
    appendRoot(snapshot, sourceRoot, "Assets", EditorAssetOrigin::Source,
               derivedRoot);
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
    rebuildLookupsAndPersist();
    refreshDirectoryWatches();
}

void EditorAssetDatabase::rebuildLookupsAndPersist()
{
    auto byPath = [](const auto& a, const auto& b) {
        return lowerAscii(a.logicalPath) < lowerAscii(b.logicalPath);
    };
    std::sort(_folders.begin(), _folders.end(), byPath);
    _folders.erase(std::unique(_folders.begin(), _folders.end(),
        [](const EditorAssetFolder& a, const EditorAssetFolder& b) {
            return lowerAscii(a.logicalPath) == lowerAscii(b.logicalPath);
        }), _folders.end());
    std::sort(_records.begin(), _records.end(), byPath);
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
    if (!_indexPath.empty()) writeIndex(_indexPath, _records);
}

void EditorAssetDatabase::refreshDirectoryWatches()
{
    if (!_watchState) return;
    const auto addRoot = [this](const std::filesystem::path& root) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) return;
        const auto add = [this](const std::filesystem::path& directory) {
            const std::string normalized = slashNormalized(
                directory.lexically_normal().string());
            const std::string key = lowerAscii(normalized);
            if (_watchState->directories.insert(key).second) {
                (void)_watchState->watcher.watch(normalized, nullptr);
            }
        };
        add(root);
        std::filesystem::recursive_directory_iterator it(root,
            std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; !error && it != end; it.increment(error)) {
            if (it->is_directory(error)) add(it->path());
        }
    };
    addRoot(_sourceRoot);
    addRoot(_derivedRoot);
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

bool EditorAssetDatabase::pollFileChanges()
{
    if (!_watchState || _projectRoot.empty()) return false;
    std::vector<ayt::io::FileWatchEvent> events;
    if (_watchState->watcher.pollPending(events) == 0u) return false;

    bool changed = false;
    bool sidecarChanged = false;
    for (const auto& event : events) {
        const std::string absolute = slashNormalized(
            std::filesystem::path(event.path).lexically_normal().string());
        const std::string sourceRelative = stripRootPrefix(absolute, _sourceRoot);
        const std::string derivedRelative = stripRootPrefix(absolute, _derivedRoot);
        const bool source = !sourceRelative.empty();
        const bool derived = !derivedRelative.empty();
        if (!source && !derived) continue;
        const std::filesystem::path path(absolute);
        if (isMetadataSidecar(path)) {
            sidecarChanged = true;
            continue;
        }
        const std::string relative = source ? sourceRelative : derivedRelative;
        const std::string logical = std::string(source ? "Assets/" : "Imported/")
            + relative;
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            const EditorAssetOrigin origin = source
                ? EditorAssetOrigin::Source : EditorAssetOrigin::Imported;
            _folders.push_back(EditorAssetFolder{
                logical, path.filename().string(), logicalParent(logical), origin});
            refreshDirectoryWatches();
            changed = true;
            continue;
        }
        const auto existing = std::find_if(_records.begin(), _records.end(),
            [&](const EditorAssetRecord& record) {
                return lowerAscii(record.logicalPath) == lowerAscii(logical);
            });
        if (!std::filesystem::is_regular_file(path, error)) {
            if (existing != _records.end()) {
                _records.erase(existing);
                changed = true;
            }
            const std::string logicalKey = lowerAscii(logical);
            const auto oldFolderCount = _folders.size();
            _folders.erase(std::remove_if(_folders.begin(), _folders.end(),
                [&](const EditorAssetFolder& folder) {
                    const std::string key = lowerAscii(folder.logicalPath);
                    return key == logicalKey
                        || (key.size() > logicalKey.size()
                            && key.rfind(logicalKey + "/", 0) == 0);
                }), _folders.end());
            const auto oldRecordCount = _records.size();
            _records.erase(std::remove_if(_records.begin(), _records.end(),
                [&](const EditorAssetRecord& record) {
                    const std::string key = lowerAscii(record.logicalPath);
                    return key.size() > logicalKey.size()
                        && key.rfind(logicalKey + "/", 0) == 0;
                }), _records.end());
            changed = changed || oldFolderCount != _folders.size()
                || oldRecordCount != _records.size();
            continue;
        }

        EditorAssetRecord record;
        record.id = stableAssetId(logical);
        record.name = path.filename().string();
        record.logicalPath = logical;
        record.absolutePath = absolute;
        record.runtimePath = absolute;
        record.type = classifyEditorAssetPath(record.name);
        record.origin = source ? EditorAssetOrigin::Source
                               : EditorAssetOrigin::Imported;
        record.size = std::filesystem::file_size(path, error);
        if (error) { error.clear(); record.size = 0; }
        const auto modified = std::filesystem::last_write_time(path, error);
        if (!error) record.lastModified = static_cast<std::int64_t>(
            modified.time_since_epoch().count());
        if (record.origin == EditorAssetOrigin::Imported) {
            record.importState = EditorAssetImportState::Ready;
        } else if (record.type == EditorAssetType::SourceModel) {
            record.importState = EditorAssetImportState::NeedsImport;
            sidecarChanged = true;
        }
        if (existing == _records.end()) _records.push_back(std::move(record));
        else *existing = std::move(record);
        changed = true;
    }

    if (sidecarChanged) {
        for (EditorAssetRecord& record : _records) {
            if (record.origin != EditorAssetOrigin::Source
                || record.type != EditorAssetType::SourceModel) continue;
            std::error_code error;
            const auto sourceModified = std::filesystem::last_write_time(
                record.absolutePath, error);
            if (error) continue;
            const std::filesystem::path sidecar =
                std::filesystem::path(_derivedRoot)
                / (std::filesystem::path(record.absolutePath).stem().string()
                    + ".aydep.json");
            const auto size = std::filesystem::file_size(sidecar, error);
            EditorAssetImportState state = EditorAssetImportState::NeedsImport;
            if (!error) {
                if (size == 0u) state = EditorAssetImportState::Failed;
                else {
                    const auto sidecarModified =
                        std::filesystem::last_write_time(sidecar, error);
                    if (!error && sidecarModified >= sourceModified) {
                        state = EditorAssetImportState::Ready;
                    }
                }
            }
            if (record.importState != state) {
                record.importState = state;
                changed = true;
            }
        }
    }
    if (changed) rebuildLookupsAndPersist();
    return changed;
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
