#include "AYEditor/EditorAssetImportQueue.h"
#include "EditorImportRecipeStore.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <AYAssetFormat/AssetFormat.h>
#include <AYIO/Env.h>
#include <AYIO/PathSafety.h>

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

void EditorAssetImportQueue::configureProject(std::string projectRoot, std::string sourceRoot)
{
    _projectRoot = std::move(projectRoot);
    _sourceRoot = std::move(sourceRoot);
}

EditorAssetImportJobId EditorAssetImportQueue::enqueue(
    const std::string& sourcePath,
    const std::string& destinationDirectory,
    bool force)
{
    ayt::resource::ImportOptions options;
    options.sourcePath = sourcePath;
    options.outputDir = destinationDirectory;
    options.requireCharacterAssets = false;
    options.loadOption = ayt::resource::IConverter::LoadOption::Full;
    const auto cook = ayt::io::env::get("AY_IMPORT_COOK_TEXTURES").value_or("");
    options.cookTextures = !cook.empty() && cook.front() != '0';
    // A successful import carries the complete recipe. Reimporting a source
    // must not silently switch its coordinate/material settings to defaults.
    const auto stem = std::filesystem::u8path(sourcePath).stem().generic_u8string();
    const std::string name(reinterpret_cast<const char*>(stem.data()), stem.size());
    const auto sidecar = std::filesystem::u8path(destinationDirectory)
        / std::filesystem::u8path(ayt::asset_format::appendSuffix(name,
            ayt::asset_format::Id::ImportDependency));
    const auto sidecarText = sidecar.generic_u8string();
    ayt::resource::ImportRecord record;
    if (ayt::resource::readImportRecord(std::string(
            reinterpret_cast<const char*>(sidecarText.data()), sidecarText.size()), record)) {
        std::error_code error;
        if (std::filesystem::equivalent(std::filesystem::u8path(sourcePath),
                std::filesystem::u8path(record.options.sourcePath), error) && !error) {
            options = record.options;
            options.sourcePath = sourcePath;
            options.outputDir = destinationDirectory;
        }
    }
    if (options.outputNamespace.empty()) {
        std::error_code error;
        const auto root = std::filesystem::u8path(destinationDirectory);
        if (std::filesystem::is_directory(root, error)) {
            std::filesystem::directory_iterator iterator(root, error), end;
            for (; !error && iterator != end; iterator.increment(error)) {
                if (!iterator->is_regular_file() || iterator->path().extension() != ".dep") continue;
                const auto path = iterator->path().generic_u8string();
                ayt::resource::ImportRecord imported;
                if (ayt::resource::readImportRecord(std::string(reinterpret_cast<const char*>(path.data()),
                        path.size()), imported)
                    && detail::sameImportSource(imported.options.sourcePath, sourcePath)) {
                    options = imported.options;
                    options.sourcePath = sourcePath;
                    options.outputDir = destinationDirectory;
                    break;
                }
            }
        }
    }
    options.force = force;
    if (!_projectRoot.empty()) {
        for (const auto& recipe : detail::loadImportRecipes(_projectRoot)) {
            if (!detail::sameImportSource(recipe.options.sourcePath, sourcePath)
                || !detail::sameImportSource(recipe.options.outputDir, destinationDirectory)) continue;
            options = recipe.options;
            options.force = force;
            break;
        }
    }
    return enqueue(options);
}

EditorAssetImportJobId EditorAssetImportQueue::enqueue(
    const ayt::resource::ImportOptions& options)
{
    const auto normalizedSourceText = std::filesystem::u8path(options.sourcePath)
        .lexically_normal().generic_u8string();
    const std::string normalizedSource(reinterpret_cast<const char*>(normalizedSourceText.data()),
        normalizedSourceText.size());
    const auto normalizedDestinationText = std::filesystem::u8path(options.outputDir)
        .lexically_normal().generic_u8string();
    const std::string normalizedDestination =
        std::string(reinterpret_cast<const char*>(normalizedDestinationText.data()),
            normalizedDestinationText.size());
    EditorAssetImportJob job;
    job.sourcePath = normalizedSource;
    job.destinationDirectory = normalizedDestination;
    job.force = options.force;
    job.options = options;
    job.options.sourcePath = normalizedSource;
    job.options.outputDir = normalizedDestination;
    if (job.options.sourceAssetRoot.empty() && !_sourceRoot.empty()) {
        try {
            if (ayt::io::path::isLexicallyWithin(std::filesystem::weakly_canonical(
                    std::filesystem::u8path(_sourceRoot)), std::filesystem::weakly_canonical(
                    std::filesystem::absolute(std::filesystem::u8path(normalizedSource)))))
                job.options.sourceAssetRoot = _sourceRoot;
        } catch (...) {} // Let the worker report an unreadable/invalid source.
    }
    if (!Importer::isSupportedExtension(normalizedSource)) {
        job.state = EditorAssetImportJobState::Failed;
        job.progress = 1.0f;
        job.message = "Unsupported import type.";
    }
    if (!job.finished() && !_projectRoot.empty()) {
        std::string error;
        if (!detail::saveImportRecipe(_projectRoot, job.options, &error)) {
            job.state = EditorAssetImportJobState::Failed;
            job.progress = 1.0f;
            job.message = error;
        }
    }
    // All queue methods run on the editor thread. Only the copied recipe is
    // consumed by the worker. Coalesce equivalent work, never swallow a newer
    // settings change or a force request behind an ordinary cache check.
    if (!job.finished()) {
        const auto recipe = ayt::resource::serializeImportRecipe(job.options);
        for (const auto& existing : _jobs) {
            if (!existing.finished() && existing.sourcePath == normalizedSource
                && existing.destinationDirectory == normalizedDestination
                && (!job.force || existing.force)
                && ayt::resource::serializeImportRecipe(existing.options) == recipe)
                return existing.id;
        }
    }
    job.id = _nextId++;
    const EditorAssetImportJobId id = job.id;
    if (job.finished()) _pendingCompletionIds.push_back(id);
    _jobs.push_back(std::move(job));
    startNext();
    return id;
}

std::size_t EditorAssetImportQueue::enqueueChangedDependencies(
    const EditorAssetDatabase& database)
{
    std::size_t count = 0;
    // Validate persisted closures on the worker, including dependencies outside
    // the project's watched roots. Unchanged imports finish as cache hits.
    for (const auto& info : database.imports()) {
        const auto before = _jobs.size();
        (void)enqueue(info.record.options);
        if (_jobs.size() != before) ++count;
    }
    for (const EditorAssetRecord& record : database.records()) {
        if (record.type != EditorAssetType::SourceModel
            || record.importState != EditorAssetImportState::NeedsImport
            || database.importForAsset(record.id) != nullptr) continue;
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
    auto options = job->options;
    options.force = force;
    return enqueue(options);
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
    // Copy worker inputs before dispatch; the worker never touches _jobs.
    // Resolve the stable ID again for editor-thread state changes.
    EditorAssetImportJobId queuedId = 0;
    ayt::resource::ImportOptions options;
    for (const EditorAssetImportJob& job : _jobs) {
        if (job.state == EditorAssetImportJobState::Queued) {
            queuedId = job.id;
            options = job.options;
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
    _future = ayt::task::launchBackground(
        [options]() {
            return Importer::importAssetFile(options);
        });
}

bool EditorAssetImportQueue::poll()
{
    _completedJobIds = std::move(_pendingCompletionIds);
    _pendingCompletionIds.clear();
    if (!_running.has_value() || !_future.valid()) {
        startNext();
        return !_completedJobIds.empty();
    }
    if (_future.wait_for(std::chrono::seconds(0))
        != std::future_status::ready) return !_completedJobIds.empty();
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
    _completedJobIds.push_back(job->id);
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
