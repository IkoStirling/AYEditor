#include "AYEditor/EditorAssetImportQueue.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace ayt::editor {

const char* editorAssetImportJobStateName(
    EditorAssetImportJobState state) noexcept
{
    switch (state) {
    case EditorAssetImportJobState::Queued: return "Queued";
    case EditorAssetImportJobState::Running: return "Importing";
    case EditorAssetImportJobState::Succeeded: return "Complete";
    case EditorAssetImportJobState::CacheHit: return "Cache reused";
    case EditorAssetImportJobState::Failed: return "Failed";
    }
    return "Unknown";
}

EditorAssetImportQueue::~EditorAssetImportQueue()
{
    if (_future.valid()) {
        try { (void)_future.get(); } catch (...) {}
    }
}

EditorAssetImportJobId EditorAssetImportQueue::enqueue(
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    bool force)
{
    const std::string normalizedSource = std::filesystem::path(sourcePath)
        .lexically_normal().string();
    const std::string normalizedDestination =
        std::filesystem::path(destinationDirectory).lexically_normal().string();
    for (const EditorAssetImportJob& job : _jobs) {
        if (!job.finished() && job.sourcePath == normalizedSource
            && job.destinationDirectory == normalizedDestination) return job.id;
    }
    EditorAssetImportJob job;
    job.id = _nextId++;
    job.sourcePath = normalizedSource;
    job.destinationDirectory = normalizedDestination;
    job.force = force;
    if (!Importer::isSupportedExtension(normalizedSource)) {
        job.state = EditorAssetImportJobState::Failed;
        job.progress = 1.0f;
        job.message = "Unsupported import type.";
    }
    const EditorAssetImportJobId id = job.id;
    _jobs.push_back(std::move(job));
    startNext();
    return id;
}

std::size_t EditorAssetImportQueue::enqueueChangedDependencies(
    const EditorAssetDatabase& database)
{
    std::size_t count = 0;
    for (const EditorAssetRecord& record : database.records()) {
        if (record.type != EditorAssetType::SourceModel
            || record.importState != EditorAssetImportState::NeedsImport) continue;
        const auto before = _jobs.size();
        (void)enqueue(record.absolutePath, database.derivedRoot(), false);
        if (_jobs.size() != before) ++count;
    }
    return count;
}

EditorAssetImportJobId EditorAssetImportQueue::retry(
    EditorAssetImportJobId id, bool force)
{
    const EditorAssetImportJob* job = find(id);
    if (job == nullptr || !job->finished()) return 0;
    const std::string source = job->sourcePath;
    const std::string destination = job->destinationDirectory;
    return enqueue(source, destination, force);
}

const EditorAssetImportJob* EditorAssetImportQueue::find(
    EditorAssetImportJobId id) const noexcept
{
    const auto it = std::find_if(_jobs.begin(), _jobs.end(),
        [id](const EditorAssetImportJob& job) { return job.id == id; });
    return it == _jobs.end() ? nullptr : &*it;
}

EditorAssetImportJob* EditorAssetImportQueue::findMutable(
    EditorAssetImportJobId id) noexcept
{
    const auto it = std::find_if(_jobs.begin(), _jobs.end(),
        [id](const EditorAssetImportJob& job) { return job.id == id; });
    return it == _jobs.end() ? nullptr : &*it;
}

void EditorAssetImportQueue::startNext()
{
    if (_running.has_value()) return;
    // B-1 (ayeditor audit 2026-09-14): grab the id + payload by-value first,
    // then re-resolve through findMutable() to flip state. Holding the
    // std::find_if iterator across the std::async launch below would be a
    // UAF trap: if a concurrent enqueue() reallocates _jobs before the
    // future returns, the iterator is invalidated and the state mutation
    // writes to freed storage. Copying by-value first and re-finding by id
    // keeps every mutation bounded to the freshly-resolved element.
    EditorAssetImportJobId queuedId = 0;
    std::string source;
    std::string destination;
    bool force = false;
    for (const EditorAssetImportJob& job : _jobs) {
        if (job.state == EditorAssetImportJobState::Queued) {
            queuedId = job.id;
            source = job.sourcePath;
            destination = job.destinationDirectory;
            force = job.force;
            break;
        }
    }
    if (queuedId == 0) return;
    if (EditorAssetImportJob* job = findMutable(queuedId)) {
        job->state = EditorAssetImportJobState::Running;
        job->progress = 0.05f;
    } else {
        return;
    }
    _running = queuedId;
    _future = std::async(std::launch::async,
        [source, destination, force]() {
            return Importer::importAssetFile(source, destination, {}, force);
        });
}

bool EditorAssetImportQueue::poll()
{
    if (!_running.has_value() || !_future.valid()) {
        startNext();
        return false;
    }
    if (_future.wait_for(std::chrono::seconds(0))
        != std::future_status::ready) return false;
    // B-1 (ayeditor audit 2026-09-14): _running now holds a job id, not a
    // vector index. Re-resolve through findMutable() right before the
    // mutation in case a concurrent enqueue() reallocated _jobs while the
    // worker future was in flight. The id lives on every element, so this
    // lookup is stable across reallocations.
    EditorAssetImportJob* job = findMutable(*_running);
    if (job == nullptr) {
        // The running job vanished from _jobs between launch and finish.
        // This should not occur because enqueue() never removes elements
        // mid-flight, but guard against it so a future regression cannot
        // crash the editor on a stale id.
        _running.reset();
        _future = {};
        startNext();
        return false;
    }
    try {
        const Importer::Result result = _future.get();
        job->progress = 1.0f;
        if (!result.success) {
            job->state = EditorAssetImportJobState::Failed;
            job->message = result.errorMessage.empty()
                ? "Importer returned no failure reason." : result.errorMessage;
        } else {
            job->state = result.usedCache
                ? EditorAssetImportJobState::CacheHit
                : EditorAssetImportJobState::Succeeded;
            job->message = result.usedCache ? "Existing import cache reused."
                                           : "Import complete.";
            for (const auto& resource : result.conversion.resources) {
                if (!resource.path.empty()) job->outputPaths.push_back(resource.path);
            }
        }
    } catch (const std::exception& error) {
        job->progress = 1.0f;
        job->state = EditorAssetImportJobState::Failed;
        job->message = error.what();
    } catch (...) {
        job->progress = 1.0f;
        job->state = EditorAssetImportJobState::Failed;
        job->message = "Importer failed with an unknown error.";
    }
    _running.reset();
    startNext();
    return true;
}

float EditorAssetImportQueue::overallProgress() const noexcept
{
    if (_jobs.empty()) return 1.0f;
    float total = 0.0f;
    for (const EditorAssetImportJob& job : _jobs) total += job.progress;
    return total / static_cast<float>(_jobs.size());
}

void EditorAssetImportQueue::clearFinished()
{
    _jobs.erase(std::remove_if(_jobs.begin(), _jobs.end(),
        [](const EditorAssetImportJob& job) { return job.finished(); }),
        _jobs.end());
}

} // namespace ayt::editor
