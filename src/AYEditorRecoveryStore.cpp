#include "AYEditor/EditorRecoveryStore.h"

#include "AYEditor/EditorExtension.h"
#include "AYEditor/EditorAssetOperations.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string_view>

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

bool endsWithCaseInsensitive(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size()) return false;
    const std::string_view tail = value.substr(value.size() - suffix.size());
    return std::equal(tail.begin(), tail.end(), suffix.begin(),
        [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        });
}

std::string documentRecoverySuffix(const IEditorDocument& document)
{
    static constexpr std::array<std::string_view, 3> compoundSuffixes = {
        ".gameflow.json", ".uiflow.json", ".ui.json",
    };

    const fs::path sourcePath(document.path());
    const std::string fileName = sourcePath.filename().string();
    for (const std::string_view suffix : compoundSuffixes) {
        if (endsWithCaseInsensitive(fileName, suffix)) {
            // Keep the spelling authored on disk while retaining every suffix
            // segment needed by asset/editor routing.
            return fileName.substr(fileName.size() - suffix.size());
        }
    }
    if (!document.path().empty()) return sourcePath.extension().string();

    // Untitled documents have no source filename to inspect. Their stable
    // editor document type supplies the canonical suffix so a restored copy is
    // immediately discoverable by the asset database.
    if (document.typeId() == "ayeditor.game-flow.document") {
        return ".gameflow.json";
    }
    if (document.typeId() == "ayeditor.ui-flow.document") {
        return ".uiflow.json";
    }
    if (document.typeId() == "ayeditor.ui-layout.document") {
        return ".ui.json";
    }
    return {};
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
            + documentRecoverySuffix(*document);
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
    std::vector<std::size_t> all;
    all.reserve(_previousEntries.size());
    for (std::size_t index = 0; index < _previousEntries.size(); ++index) {
        all.push_back(index);
    }
    return restorePrevious(all);
}

std::vector<EditorRecoveryDocument>
EditorRecoveryStore::recoverableDocuments() const
{
    std::vector<EditorRecoveryDocument> result;
    result.reserve(_previousEntries.size());
    for (const Entry& entry : _previousEntries) {
        result.push_back(EditorRecoveryDocument{
            entry.originalPath,
            (fs::path(_previousRoot) / "documents" / entry.recoveryName).string(),
            entry.title});
    }
    return result;
}

EditorRecoveryResult EditorRecoveryStore::restorePrevious(
    const std::vector<std::size_t>& documentIndices)
{
    EditorRecoveryResult result;
    if (_previousRoot.empty()) {
        result.error = "There is no crashed editor session to restore.";
        return result;
    }
    if (documentIndices.empty()) {
        result.error = "No recovery documents were selected.";
        return result;
    }
    std::vector<std::size_t> selected = documentIndices;
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected.back() >= _previousEntries.size()) {
        result.error = "Recovery document selection is invalid.";
        return result;
    }
    for (const std::size_t index : selected) {
        const Entry& entry = _previousEntries[index];
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
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        (void)appendEditorAssetOperationHistory(_projectRoot,
            EditorAssetOperationHistoryEntry{
                timestamp, "Recovery", recovery.string(),
                destination.string(), {}});
    }
    for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
        std::error_code ignored;
        fs::remove(fs::path(_previousRoot) / "documents"
            / _previousEntries[*it].recoveryName, ignored);
        _previousEntries.erase(_previousEntries.begin()
            + static_cast<std::ptrdiff_t>(*it));
    }
    if (_previousEntries.empty()) {
        std::error_code ignored;
        fs::remove_all(_previousRoot, ignored);
        _previousRoot.clear();
    } else {
        std::string manifestError;
        if (!writePreviousManifest(&manifestError)) result.error = manifestError;
    }
    return result;
}

bool EditorRecoveryStore::writePreviousManifest(std::string* error)
{
    if (error != nullptr) error->clear();
    if (_previousRoot.empty()) return false;
    const fs::path path = fs::path(_previousRoot) / "manifest.tsv";
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "AYEDITOR_RECOVERY\t1\n";
    for (const Entry& entry : _previousEntries) {
        output << std::quoted(entry.originalPath) << '\t'
               << std::quoted(entry.recoveryName) << '\t'
               << std::quoted(entry.title) << '\n';
    }
    output.close();
    if (!output) {
        if (error != nullptr) *error = "Could not update recovery manifest.";
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

bool EditorRecoveryStore::discardPrevious(std::string* error)
{
    if (error != nullptr) error->clear();
    if (_previousRoot.empty()) return true;
    std::error_code ioError;
    fs::remove_all(_previousRoot, ioError);
    if (ioError) {
        if (error != nullptr) *error = ioError.message();
        return false;
    }
    _previousRoot.clear();
    _previousEntries.clear();
    return true;
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
