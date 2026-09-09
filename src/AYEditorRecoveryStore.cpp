#include "AYEditor/EditorRecoveryStore.h"

#include "AYEditor/EditorExtension.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

std::uint64_t stableHash(const std::string& value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool copyReplacing(const fs::path& from, const fs::path& to,
                   std::error_code& error)
{
    fs::create_directories(to.parent_path(), error);
    if (error) return false;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    return !error;
}

} // namespace

EditorRecoveryStore::EditorRecoveryStore(std::string projectRoot)
{
    setProjectRoot(std::move(projectRoot));
}

EditorRecoveryStore::~EditorRecoveryStore() = default;

void EditorRecoveryStore::setProjectRoot(std::string projectRoot)
{
    std::error_code error;
    const fs::path root = projectRoot.empty()
        ? fs::path{} : fs::absolute(projectRoot, error).lexically_normal();
    _projectRoot = error ? std::string{} : root.string();
    _currentRoot.clear();
    _previousRoot.clear();
    _currentEntries.clear();
    _previousEntries.clear();
    _begun = false;
    _clean = false;
}

bool EditorRecoveryStore::beginSession(std::string* error)
{
    if (error != nullptr) error->clear();
    if (_projectRoot.empty()) {
        if (error != nullptr) *error = "Project root is unavailable.";
        return false;
    }
    const fs::path editorRoot = fs::path(_projectRoot) / ".ayeditor";
    const fs::path current = editorRoot / "recovery";
    std::error_code ioError;
    if (fs::exists(current / "session.lock", ioError)) {
        const auto serial = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const fs::path previous = editorRoot
            / ("recovery-crash-" + std::to_string(serial));
        fs::rename(current, previous, ioError);
        if (ioError) {
            if (error != nullptr) *error = "Could not preserve crash recovery: "
                + ioError.message();
            return false;
        }
        _previousRoot = previous.string();
        (void)loadPreviousManifest();
    } else if (fs::exists(current, ioError)) {
        fs::remove_all(current, ioError);
    }
    ioError.clear();
    fs::create_directories(current / "documents", ioError);
    if (ioError) {
        if (error != nullptr) *error = "Could not create recovery folder: "
            + ioError.message();
        return false;
    }
    std::ofstream lock(current / "session.lock", std::ios::binary);
    lock << "AYEDITOR_SESSION\n";
    if (!lock) {
        if (error != nullptr) *error = "Could not create recovery lock.";
        return false;
    }
    _currentRoot = current.string();
    _begun = true;
    return true;
}

EditorRecoveryResult EditorRecoveryStore::autosave(
    const std::vector<const IEditorDocument*>& documents)
{
    EditorRecoveryResult result;
    if (!_begun) {
        result.error = "Recovery session is not active.";
        return result;
    }
    std::vector<Entry> entries;
    for (const IEditorDocument* document : documents) {
        if (document == nullptr || !document->isDirty()) continue;
        const std::string identity = document->path().empty()
            ? document->typeId() + ":" + document->title()
            : document->path();
        const std::string name = std::to_string(stableHash(identity))
            + std::filesystem::path(document->path()).extension().string();
        const fs::path destination = fs::path(_currentRoot)
            / "documents" / name;
        std::string saveError;
        if (!document->writeRecoveryCopy(destination.string(), &saveError)) {
            if (!saveError.empty() && result.error.empty()) {
                result.error = document->title() + ": " + saveError;
            }
            continue;
        }
        entries.push_back({document->path(), name, document->title()});
        ++result.documents;
    }
    _currentEntries = std::move(entries);
    std::string manifestError;
    if (!writeCurrentManifest(&manifestError) && result.error.empty()) {
        result.error = std::move(manifestError);
    }
    return result;
}

bool EditorRecoveryStore::writeCurrentManifest(std::string* error)
{
    const fs::path path = fs::path(_currentRoot) / "manifest.tsv";
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "AYEDITOR_RECOVERY\t1\n";
    for (const Entry& entry : _currentEntries) {
        output << std::quoted(entry.originalPath) << '\t'
               << std::quoted(entry.recoveryName) << '\t'
               << std::quoted(entry.title) << '\n';
    }
    output.close();
    if (!output) {
        if (error != nullptr) *error = "Could not write recovery manifest.";
        return false;
    }
    std::error_code ioError;
    fs::rename(temporary, path, ioError);
    if (ioError) {
        fs::remove(path, ioError);
        ioError.clear();
        fs::rename(temporary, path, ioError);
    }
    if (ioError && error != nullptr) *error = ioError.message();
    return !ioError;
}

bool EditorRecoveryStore::loadPreviousManifest()
{
    _previousEntries.clear();
    std::ifstream input(fs::path(_previousRoot) / "manifest.tsv",
                        std::ios::binary);
    std::string marker;
    if (!std::getline(input, marker) || marker != "AYEDITOR_RECOVERY\t1") {
        _previousRoot.clear();
        return false;
    }
    Entry entry;
    while (input >> std::quoted(entry.originalPath)
                 >> std::quoted(entry.recoveryName)
                 >> std::quoted(entry.title)) {
        if (fs::is_regular_file(fs::path(_previousRoot) / "documents"
                / entry.recoveryName)) {
            _previousEntries.push_back(entry);
        }
    }
    if (_previousEntries.empty()) _previousRoot.clear();
    return !_previousEntries.empty();
}

EditorRecoveryResult EditorRecoveryStore::restorePrevious()
{
    EditorRecoveryResult result;
    if (_previousRoot.empty()) {
        result.error = "There is no crashed editor session to restore.";
        return result;
    }
    for (const Entry& entry : _previousEntries) {
        const fs::path recovery = fs::path(_previousRoot) / "documents"
            / entry.recoveryName;
        fs::path destination = entry.originalPath;
        if (destination.empty()) {
            destination = fs::path(_projectRoot) / "Assets" / "Recovered"
                / entry.recoveryName;
        }
        std::error_code error;
        if (fs::exists(destination, error)) {
            const fs::path backup = destination.string() + ".before-recovery";
            if (!copyReplacing(destination, backup, error)) {
                result.error = "Could not back up " + destination.string()
                    + ": " + error.message();
                return result;
            }
        }
        if (!copyReplacing(recovery, destination, error)) {
            result.error = "Could not restore " + destination.string()
                + ": " + error.message();
            return result;
        }
        ++result.documents;
    }
    std::error_code ignored;
    fs::remove_all(_previousRoot, ignored);
    _previousRoot.clear();
    _previousEntries.clear();
    return result;
}

void EditorRecoveryStore::markCleanShutdown()
{
    if (!_begun || _clean) return;
    std::error_code ignored;
    fs::remove_all(_currentRoot, ignored);
    _currentEntries.clear();
    _clean = true;
}

} // namespace ayt::editor
