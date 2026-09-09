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

void EditorAssetImportQueue::startNext()
{
    if (_running.has_value()) return;
    const auto it = std::find_if(_jobs.begin(), _jobs.end(),
        [](const EditorAssetImportJob& job) {
            return job.state == EditorAssetImportJobState::Queued;
        });
    if (it == _jobs.end()) return;
    const std::size_t index = static_cast<std::size_t>(it - _jobs.begin());
    _running = index;
    EditorAssetImportJob& job = _jobs[index];
    job.state = EditorAssetImportJobState::Running;
    job.progress = 0.05f;
    const std::string source = job.sourcePath;
    const std::string destination = job.destinationDirectory;
    const bool force = job.force;
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
    EditorAssetImportJob& job = _jobs[*_running];
    try {
        const Importer::Result result = _future.get();
        job.progress = 1.0f;
        if (!result.success) {
            job.state = EditorAssetImportJobState::Failed;
            job.message = result.errorMessage.empty()
                ? "Importer returned no failure reason." : result.errorMessage;
        } else {
            job.state = result.usedCache
                ? EditorAssetImportJobState::CacheHit
                : EditorAssetImportJobState::Succeeded;
            job.message = result.usedCache ? "Existing import cache reused."
                                           : "Import complete.";
            for (const auto& resource : result.conversion.resources) {
                if (!resource.path.empty()) job.outputPaths.push_back(resource.path);
            }
        }
    } catch (const std::exception& error) {
        job.progress = 1.0f;
        job.state = EditorAssetImportJobState::Failed;
        job.message = error.what();
    } catch (...) {
        job.progress = 1.0f;
        job.state = EditorAssetImportJobState::Failed;
        job.message = "Importer failed with an unknown error.";
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
