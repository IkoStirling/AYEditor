#include "AYEditor/EditorAssetTrash.h"
#include "AYEditor/EditorAssetOperations.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace ayt::editor {
namespace {

bool moveFile(const std::filesystem::path& from,
              const std::filesystem::path& to,
              std::error_code& error)
{
    std::filesystem::create_directories(to.parent_path(), error);
    if (error) return false;
    std::filesystem::rename(from, to, error);
    if (!error) return true;
    error.clear();
    std::filesystem::copy_file(
        from, to, std::filesystem::copy_options::none, error);
    if (error) return false;
    std::filesystem::remove(from, error);
    return !error;
}

bool isInside(const std::filesystem::path& child,
              const std::filesystem::path& parent)
{
    const auto relative = child.lexically_relative(parent);
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
}

bool writeManifest(const std::filesystem::path& transaction,
                   const std::vector<std::pair<std::string, std::string>>& entries,
                   std::error_code& error)
{
    std::filesystem::create_directories(transaction, error);
    if (error) return false;
    const auto temporary = transaction / "manifest.tsv.tmp";
    const auto manifest = transaction / "manifest.tsv";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    output << "AYEDITOR_TRASH\t1\n";
    for (const auto& entry : entries) {
        output << std::quoted(entry.first) << '\t'
               << std::quoted(entry.second) << '\n';
    }
    output.close();
    if (!output) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    std::filesystem::rename(temporary, manifest, error);
    return !error;
}

} // namespace

EditorAssetTrash::EditorAssetTrash(std::string projectRoot)
{
    setProjectRoot(std::move(projectRoot));
}

void EditorAssetTrash::setProjectRoot(std::string projectRoot)
{
    std::error_code ec;
    std::filesystem::path root = projectRoot.empty()
        ? std::filesystem::path{} : std::filesystem::absolute(projectRoot, ec);
    _projectRoot = ec ? std::string{} : root.lexically_normal().string();
    reload();
}

void EditorAssetTrash::reload()
{
    _transactions.clear();
    if (_projectRoot.empty()) return;

    std::error_code ec;
    const std::filesystem::path trashRoot =
        std::filesystem::path(_projectRoot) / ".ayeditor" / "trash";
    std::vector<std::filesystem::path> transactions;
    for (std::filesystem::directory_iterator it(trashRoot, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) transactions.push_back(it->path());
    }
    ec.clear();
    std::sort(transactions.begin(), transactions.end(), std::greater<>());
    for (const auto& transaction : transactions) {
        std::ifstream input(transaction / "manifest.tsv", std::ios::binary);
        std::string marker;
        if (!std::getline(input, marker) || marker != "AYEDITOR_TRASH\t1") {
            continue;
        }
        EditorAssetTrashTransaction loaded;
        loaded.id = transaction.filename().string();
        std::string original;
        std::string trashed;
        while (input >> std::quoted(original) >> std::quoted(trashed)) {
            if (!std::filesystem::exists(trashed)) {
                loaded.entries.clear();
                break;
            }
            loaded.entries.push_back({original, trashed});
        }
        if (!loaded.entries.empty()) _transactions.push_back(std::move(loaded));
    }
}

EditorAssetTrashResult EditorAssetTrash::moveToTrash(
    const std::vector<EditorAssetRecord>& records)
{
    EditorAssetTrashResult result;
    if (_projectRoot.empty()) {
        result.error = "Project root is unavailable.";
        return result;
    }
    const std::filesystem::path root(_projectRoot);
    const auto serial = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::filesystem::path transaction = root / ".ayeditor" / "trash"
        / std::to_string(serial);
    std::vector<EditorAssetTrashEntry> moved;
    for (const EditorAssetRecord& record : records) {
        const std::filesystem::path original =
            std::filesystem::absolute(record.absolutePath).lexically_normal();
        if (!isInside(original, root)) {
            result.error = "Refusing to delete an asset outside the project: "
                + original.string();
            break;
        }
        const std::filesystem::path relative = original.lexically_relative(root);
        const std::filesystem::path destination = transaction / relative;
        std::error_code error;
        if (!moveFile(original, destination, error)) {
            result.error = record.logicalPath + ": " + error.message();
            break;
        }
        moved.push_back({original.string(), destination.string()});
    }
    if (!result.error.empty()) {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ignored;
            (void)moveFile(it->trashPath, it->originalPath, ignored);
        }
        return result;
    }
    std::vector<std::pair<std::string, std::string>> manifestEntries;
    manifestEntries.reserve(moved.size());
    for (const EditorAssetTrashEntry& entry : moved) {
        manifestEntries.emplace_back(entry.originalPath, entry.trashPath);
    }
    std::error_code manifestError;
    if (!writeManifest(transaction, manifestEntries, manifestError)) {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ignored;
            (void)moveFile(it->trashPath, it->originalPath, ignored);
        }
        result.error = "Could not persist trash transaction: "
            + manifestError.message();
        return result;
    }
    result.moved = moved.size();
    for (const EditorAssetTrashEntry& entry : moved) {
        (void)appendEditorAssetOperationHistory(_projectRoot,
            EditorAssetOperationHistoryEntry{
                serial, "Delete", entry.originalPath, entry.trashPath, {}});
    }
    reload();
    return result;
}

EditorAssetTrashResult EditorAssetTrash::restoreLast()
{
    if (_transactions.empty()) {
        return {0u, "There is no deleted asset transaction to restore."};
    }
    return restore(_transactions.front().id);
}

EditorAssetTrashResult EditorAssetTrash::restore(
    const std::string& transactionId)
{
    EditorAssetTrashResult result;
    const auto transaction = std::find_if(
        _transactions.begin(), _transactions.end(),
        [&](const EditorAssetTrashTransaction& candidate) {
            return candidate.id == transactionId;
        });
    if (transaction == _transactions.end()) {
        result.error = "There is no deleted asset transaction to restore.";
        return result;
    }
    for (const EditorAssetTrashEntry& entry : transaction->entries) {
        if (std::filesystem::exists(entry.originalPath)) {
            result.error = "Restore destination already exists: "
                + entry.originalPath;
            return result;
        }
    }
    std::vector<EditorAssetTrashEntry> restored;
    for (const EditorAssetTrashEntry& entry : transaction->entries) {
        std::error_code error;
        if (!moveFile(entry.trashPath, entry.originalPath, error)) {
            result.error = entry.originalPath + ": " + error.message();
            for (auto it = restored.rbegin(); it != restored.rend(); ++it) {
                std::error_code ignored;
                (void)moveFile(it->originalPath, it->trashPath, ignored);
            }
            return result;
        }
        restored.push_back(entry);
    }
    result.moved = restored.size();
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const EditorAssetTrashEntry& entry : restored) {
        (void)appendEditorAssetOperationHistory(_projectRoot,
            EditorAssetOperationHistoryEntry{
                timestamp, "Restore", entry.trashPath,
                entry.originalPath, {}});
    }
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(_projectRoot)
        / ".ayeditor" / "trash" / transactionId, ignored);
    reload();
    return result;
}

EditorAssetTrashResult EditorAssetTrash::purge(
    const std::string& transactionId)
{
    EditorAssetTrashResult result;
    const auto transaction = std::find_if(
        _transactions.begin(), _transactions.end(),
        [&](const EditorAssetTrashTransaction& candidate) {
            return candidate.id == transactionId;
        });
    if (transaction == _transactions.end()) {
        result.error = "Trash transaction does not exist.";
        return result;
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto entries = transaction->entries;
    // B-10 (ayeditor audit 2026-09-14): re-resolve the transaction path
    // through weakly_canonical and confirm it is still inside the project
    // root before invoking remove_all. Without this guard a symlink that
    // escaped `_projectRoot` (or a hostile project descriptor pointing at
    // an arbitrary tree) could make purge() delete files outside the
    // project. The transactionId itself is loaded from disk in reload(),
    // so we cannot trust it by string alone.
    std::error_code canonicalError;
    const std::filesystem::path transactionPath =
        std::filesystem::weakly_canonical(
            std::filesystem::path(_projectRoot) / ".ayeditor" / "trash"
                / transactionId,
            canonicalError);
    if (canonicalError) {
        result.error = "Could not resolve trash transaction path: "
            + canonicalError.message();
        return result;
    }
    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(
            std::filesystem::path(_projectRoot), canonicalError);
    if (canonicalError) {
        result.error = "Could not resolve project root: "
            + canonicalError.message();
        return result;
    }
    if (!isInside(transactionPath, canonicalRoot)) {
        result.error = "Refusing to purge a trash transaction outside the "
            "project root: " + transactionPath.string();
        return result;
    }
    std::error_code error;
    std::filesystem::remove_all(transactionPath, error);
    if (error) {
        result.error = "Could not permanently clear trash: " + error.message();
        return result;
    }
    result.moved = entries.size();
    for (const EditorAssetTrashEntry& entry : entries) {
        (void)appendEditorAssetOperationHistory(_projectRoot,
            EditorAssetOperationHistoryEntry{
                timestamp, "Purge", entry.trashPath, {}, {}});
    }
    reload();
    return result;
}

EditorAssetTrashResult EditorAssetTrash::purgeAll()
{
    EditorAssetTrashResult result;
    const std::vector<EditorAssetTrashTransaction> transactions = _transactions;
    for (const EditorAssetTrashTransaction& transaction : transactions) {
        const EditorAssetTrashResult current = purge(transaction.id);
        if (!current) {
            result.error = current.error;
            return result;
        }
        result.moved += current.moved;
    }
    return result;
}

} // namespace ayt::editor
