#pragma once

#include "AYEditor/EditorVersion.h"

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor {

using EditorAssetId = std::uint64_t;

enum class EditorAssetType : std::uint8_t {
    Unknown = 0,
    Mesh,
    Material,
    Texture,
    Scene,
    Animation,
    Skeleton,
    Script,
    Shader,
    Audio,
    UiLayout,
    SourceModel,
    // Appended to preserve the numeric values of the public asset-type ABI.
    Tilemap,
    UiFlow,
    GameFlow,
    // Author-side humanoid mapping resource. Appended for ABI stability.
    SkeletonMapping,
};

enum class EditorAssetOrigin : std::uint8_t {
    Source = 0,
    Imported,
};

enum class EditorAssetImportState : std::uint8_t {
    NotApplicable = 0,
    NeedsImport,
    Ready,
    Failed,
};

struct EditorAssetRecord {
    EditorAssetId id = 0;
    std::string name;
    // Project-browser path. Always slash-normalized and rooted at either
    // "Assets" or "Imported".
    std::string logicalPath;
    std::string absolutePath;
    // Path consumed by AYResource. P0 uses an absolute loose-file path so it
    // does not depend on the process-wide runtime asset root.
    std::string runtimePath;
    EditorAssetType type = EditorAssetType::Unknown;
    EditorAssetOrigin origin = EditorAssetOrigin::Source;
    std::uintmax_t size = 0;
    std::int64_t lastModified = 0;
    // Appended so consumers built against the earlier record layout retain
    // all existing field offsets. Source-model state is also persisted in the
    // editor index for fast project status display.
    EditorAssetImportState importState = EditorAssetImportState::NotApplicable;
};

struct EditorAssetFolder {
    std::string logicalPath;
    std::string displayName;
    std::string parentPath;
    EditorAssetOrigin origin = EditorAssetOrigin::Source;
};

struct EditorAssetEntry {
    bool folder = false;
    std::string folderPath;
    EditorAssetId assetId = 0;
    std::string displayName;
    EditorAssetType type = EditorAssetType::Unknown;
    EditorAssetOrigin origin = EditorAssetOrigin::Source;
};

const char* editorAssetTypeName(EditorAssetType type) noexcept;
const char* editorAssetImportStateName(EditorAssetImportState state) noexcept;
EditorAssetType classifyEditorAssetPath(const std::string& path);

// AYEditor-owned catalog for loose project files. This is deliberately not a
// second ResourceManager: it only enumerates/searches files and supplies stable
// editor IDs; runtime decoding/loading remains AYResource's responsibility.
class EditorAssetDatabase {
public:
    EditorAssetDatabase();
    ~EditorAssetDatabase();

    EditorAssetDatabase(const EditorAssetDatabase&) = delete;
    EditorAssetDatabase& operator=(const EditorAssetDatabase&) = delete;

    bool open(const std::string& projectRoot, std::string* error = nullptr);
    void close();

    bool scanNow(std::string* error = nullptr);
    bool requestScan();
    // Applies a completed asynchronous snapshot. Returns true only when a new
    // snapshot was committed (successfully or with lastError populated).
    bool pollScan();
    bool scanPending() const noexcept { return _scanPending; }
    // Drain filesystem notifications and update only the affected records.
    // Returns true when the visible catalog changed.
    bool pollFileChanges();
    bool loadedFromIndex() const noexcept { return _loadedFromIndex; }

    const std::string& projectRoot() const noexcept { return _projectRoot; }
    const std::string& sourceRoot() const noexcept { return _sourceRoot; }
    const std::string& derivedRoot() const noexcept { return _derivedRoot; }
    const std::string& lastError() const noexcept { return _lastError; }
    const std::string& indexPath() const noexcept { return _indexPath; }

    const std::vector<EditorAssetRecord>& records() const noexcept {
        return _records;
    }
    const std::vector<EditorAssetFolder>& folders() const noexcept {
        return _folders;
    }

    const EditorAssetRecord* find(EditorAssetId id) const noexcept;
    const EditorAssetRecord* findByLogicalPath(
        const std::string& logicalPath) const noexcept;

    // Return the slash-normalized, asset-root-relative reference written to
    // scene component fields. Editor preview/loading may continue to use the
    // record's absolute runtimePath, but authored scenes must remain portable
    // when a project is moved to another directory or machine.
    std::string portableAssetPath(const EditorAssetRecord& record) const;
    std::string portableAssetPath(const std::string& path) const;

    // Empty query lists direct children. A non-empty query searches
    // recursively beneath folderPath while keeping folders before files.
    std::vector<EditorAssetEntry> entries(
        const std::string& folderPath,
        const std::string& query = {},
        std::optional<EditorAssetType> type = std::nullopt) const;

private:
    struct WatchState;
    struct Snapshot {
        std::vector<EditorAssetRecord> records;
        std::vector<EditorAssetFolder> folders;
        std::string error;
    };

    static Snapshot scanRoots(const std::filesystem::path& sourceRoot,
                              const std::filesystem::path& derivedRoot);
    void applySnapshot(Snapshot snapshot);
    void rebuildLookupsAndPersist();
    void refreshDirectoryWatches();

    std::string _projectRoot;
    std::string _sourceRoot;
    std::string _derivedRoot;
    std::string _lastError;
    std::string _indexPath;
    std::vector<EditorAssetRecord> _records;
    std::vector<EditorAssetFolder> _folders;
    std::unordered_map<EditorAssetId, std::size_t> _recordById;
    std::unordered_map<std::string, std::size_t> _recordByLogicalPath;
    std::future<Snapshot> _scanFuture;
    std::unique_ptr<WatchState> _watchState;
    bool _scanPending = false;
    bool _loadedFromIndex = false;
};

} // namespace ayt::editor
