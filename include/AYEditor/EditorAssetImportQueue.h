#pragma once

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/Importer.h"

#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace ayt::editor {

using EditorAssetImportJobId = std::uint64_t;

enum class EditorAssetImportJobState : std::uint8_t {
    Queued = 0,
    Running,
    Succeeded,
    CacheHit,
    Failed,
};

struct EditorAssetImportJob {
    EditorAssetImportJobId id = 0;
    std::string sourcePath;
    std::string destinationDirectory;
    EditorAssetImportJobState state = EditorAssetImportJobState::Queued;
    float progress = 0.0f;
    bool force = false;
    std::string message;
    std::vector<std::string> outputPaths;

    bool finished() const noexcept {
        return state == EditorAssetImportJobState::Succeeded
            || state == EditorAssetImportJobState::CacheHit
            || state == EditorAssetImportJobState::Failed;
    }
};

const char* editorAssetImportJobStateName(
    EditorAssetImportJobState state) noexcept;

// Project-scoped sequential queue. Conversion runs away from the UI thread,
// while poll() commits results on the editor thread. Sequential work avoids
// several heavy model conversions competing for memory and disk I/O.
class EditorAssetImportQueue final {
public:
    EditorAssetImportQueue() = default;
    ~EditorAssetImportQueue();

    EditorAssetImportQueue(const EditorAssetImportQueue&) = delete;
    EditorAssetImportQueue& operator=(const EditorAssetImportQueue&) = delete;

    EditorAssetImportJobId enqueue(const std::string& sourcePath,
                                   const std::string& destinationDirectory,
                                   bool force = false);
    std::size_t enqueueChangedDependencies(
        const EditorAssetDatabase& database);
    EditorAssetImportJobId retry(EditorAssetImportJobId id,
                                 bool force = true);
    bool poll();

    const std::vector<EditorAssetImportJob>& jobs() const noexcept {
        return _jobs;
    }
    const EditorAssetImportJob* find(EditorAssetImportJobId id) const noexcept;
    bool busy() const noexcept { return _running.has_value(); }
    float overallProgress() const noexcept;
    void clearFinished();

private:
    void startNext();

    std::vector<EditorAssetImportJob> _jobs;
    std::optional<std::size_t> _running;
    std::future<Importer::Result> _future;
    EditorAssetImportJobId _nextId = 1;
};

} // namespace ayt::editor
