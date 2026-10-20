#pragma once

#include "AYEditor/EditorAssetDatabase.h"
#include "AYEditor/Importer.h"

#include <cstdint>
#include <future>
#include <AYTask/BackgroundJob.h>
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
    ayt::resource::ImportOptions options;

    bool finished() const noexcept {
        return state == EditorAssetImportJobState::Succeeded
            || state == EditorAssetImportJobState::CacheHit
            || state == EditorAssetImportJobState::Failed;
    }
};

const char* editorAssetImportJobStateName(
    EditorAssetImportJobState state) noexcept;

// Project-scoped sequential queue. All methods belong to the editor thread.
// Conversion runs away from the UI thread,
// while poll() commits results on the editor thread. Sequential work avoids
// several heavy model conversions competing for memory and disk I/O.
class EditorAssetImportQueue final {
public:
    EditorAssetImportQueue() = default;
    ~EditorAssetImportQueue();

    EditorAssetImportQueue(const EditorAssetImportQueue&) = delete;
    EditorAssetImportQueue& operator=(const EditorAssetImportQueue&) = delete;

    /// Configure durable authoring settings outside the derived project cache.
    void configureProject(std::string projectRoot, std::string sourceRoot);

    EditorAssetImportJobId enqueue(const std::string& sourcePath,
                                   const std::string& destinationDirectory,
                                   bool force = false);
    /// Explicit recipe entry; ordinary source/destination enqueue recovers the
    /// last successful recipe when the source identity matches.
    EditorAssetImportJobId enqueue(const ayt::resource::ImportOptions& options);
    std::size_t enqueueChangedDependencies(
        const EditorAssetDatabase& database);
    EditorAssetImportJobId retry(EditorAssetImportJobId id,
                                 bool force = true);
    bool poll();
    /// Every completion delivered by the last poll, in completion order.
    /// Includes immediate validation failures queued before that poll.
    const std::vector<EditorAssetImportJobId>& completedJobIds() const noexcept {
        return _completedJobIds;
    }

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
    // Stable owner-thread identity; worker inputs are independent copies.
    std::optional<EditorAssetImportJobId> _running;
    ayt::task::BackgroundJob<Importer::Result> _future;
    EditorAssetImportJobId _nextId = 1;
    std::vector<EditorAssetImportJobId> _pendingCompletionIds;
    std::vector<EditorAssetImportJobId> _completedJobIds;
    std::string _projectRoot, _sourceRoot;
};

} // namespace ayt::editor
