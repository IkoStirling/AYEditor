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
    EditorAssetImportJob* findMutable(EditorAssetImportJobId id) noexcept;

    std::vector<EditorAssetImportJob> _jobs;
    // B-1 (ayeditor audit 2026-09-14): previously a std::size_t index into
    // _jobs. Holding the index was a UAF trap: concurrent enqueue() calls
    // (driven by the asset DB scan thread) can reallocate _jobs while the
    // worker future is still in flight, which left _jobs[*_running] pointing
    // at freed storage. The id is stable across reallocations because it is
    // stored on every element of _jobs, so the consumer re-resolves it
    // through findMutable() right before it touches the element.
    std::optional<EditorAssetImportJobId> _running;
    std::future<Importer::Result> _future;
    EditorAssetImportJobId _nextId = 1;
};

} // namespace ayt::editor
