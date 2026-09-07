#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
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
};

enum class EditorAssetOrigin : std::uint8_t {
    Source = 0,
    Imported,
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
EditorAssetType classifyEditorAssetPath(const std::string& path);

// AYEditor-owned catalog for loose project files. This is deliberately not a
// second ResourceManager: it only enumerates/searches files and supplies stable
// editor IDs; runtime decoding/loading remains AYResource's responsibility.
class EditorAssetDatabase {
public:
    EditorAssetDatabase() = default;
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

    const std::string& projectRoot() const noexcept { return _projectRoot; }
    const std::string& sourceRoot() const noexcept { return _sourceRoot; }
    const std::string& derivedRoot() const noexcept { return _derivedRoot; }
    const std::string& lastError() const noexcept { return _lastError; }

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
    struct Snapshot {
        std::vector<EditorAssetRecord> records;
        std::vector<EditorAssetFolder> folders;
        std::string error;
    };

    static Snapshot scanRoots(const std::filesystem::path& sourceRoot,
                              const std::filesystem::path& derivedRoot);
    void applySnapshot(Snapshot snapshot);

    std::string _projectRoot;
    std::string _sourceRoot;
    std::string _derivedRoot;
    std::string _lastError;
    std::vector<EditorAssetRecord> _records;
    std::vector<EditorAssetFolder> _folders;
    std::unordered_map<EditorAssetId, std::size_t> _recordById;
    std::unordered_map<std::string, std::size_t> _recordByLogicalPath;
    std::future<Snapshot> _scanFuture;
    bool _scanPending = false;
};

} // namespace ayt::editor
