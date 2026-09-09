#include "AYEditor/EditorAssetTrash.h"

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
    _lastTransactionPath.clear();
    _last.clear();
    if (_projectRoot.empty()) return;

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
        std::vector<Entry> loaded;
        std::string original;
        std::string trashed;
        while (input >> std::quoted(original) >> std::quoted(trashed)) {
            if (!std::filesystem::exists(trashed)) {
                loaded.clear();
                break;
            }
            loaded.push_back({original, trashed});
        }
        if (!loaded.empty()) {
            _last = std::move(loaded);
            _lastTransactionPath = transaction.string();
            break;
        }
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
    std::vector<Entry> moved;
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
    for (const Entry& entry : moved) {
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
    _last = std::move(moved);
    _lastTransactionPath = transaction.string();
    result.moved = _last.size();
    return result;
}

EditorAssetTrashResult EditorAssetTrash::restoreLast()
{
    EditorAssetTrashResult result;
    if (_last.empty()) {
        result.error = "There is no deleted asset transaction to restore.";
        return result;
    }
    for (const Entry& entry : _last) {
        if (std::filesystem::exists(entry.originalPath)) {
            result.error = "Restore destination already exists: "
                + entry.originalPath;
            return result;
        }
    }
    std::vector<Entry> restored;
    for (const Entry& entry : _last) {
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
    _last.clear();
    if (!_lastTransactionPath.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(_lastTransactionPath, ignored);
        _lastTransactionPath.clear();
    }
    return result;
}

} // namespace ayt::editor
