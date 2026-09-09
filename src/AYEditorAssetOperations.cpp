#include "AYEditor/EditorAssetOperations.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

std::string slashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool inside(const fs::path& child, const fs::path& parent)
{
    const fs::path relative = child.lexically_relative(parent);
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
}

bool validFileName(const std::string& name)
{
    return !name.empty() && name != "." && name != ".."
        && name.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

bool isReferenceText(const fs::path& path, std::uintmax_t size)
{
    if (size > 4u * 1024u * 1024u) return false;
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    static constexpr const char* suffixes[] = {
        ".ayscene", ".aymat", ".ui.json", ".aytilemap",
        ".aytilemap.json", ".json", ".toml", ".yaml", ".yml",
        ".lua", ".js", ".ts", ".glsl", ".vert", ".frag"
    };
    for (const char* suffix : suffixes) {
        const std::size_t length = std::char_traits<char>::length(suffix);
        if (name.size() >= length
            && name.compare(name.size() - length, length, suffix) == 0) {
            return true;
        }
    }
    return false;
}

std::string readFile(const fs::path& path, std::error_code& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = std::make_error_code(std::errc::io_error);
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), {});
}

bool writeFile(const fs::path& path, const std::string& text,
               std::error_code& error)
{
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    return true;
}

std::size_t replaceAll(std::string& text, const std::string& from,
                       const std::string& to)
{
    if (from.empty() || from == to) return 0;
    std::size_t count = 0;
    for (std::size_t position = 0;
         (position = text.find(from, position)) != std::string::npos;) {
        text.replace(position, from.size(), to);
        position += to.size();
        ++count;
    }
    return count;
}

struct MoveEntry {
    fs::path from;
    fs::path to;
    std::string oldLogical;
    std::string newLogical;
    std::string oldPortable;
    std::string newPortable;
};

EditorAssetOperationResult relocate(
    const std::string& projectRoot,
    const EditorAssetDatabase& database,
    std::vector<MoveEntry> entries,
    bool copying)
{
    EditorAssetOperationResult result;
    if (projectRoot.empty()) {
        result.error = "Project root is unavailable.";
        return result;
    }
    const fs::path root(projectRoot);
    for (const MoveEntry& entry : entries) {
        if (!inside(entry.from, root) || !inside(entry.to, root)) {
            result.error = "Asset operation escaped the project root.";
            return result;
        }
        if (!fs::is_regular_file(entry.from)) {
            result.error = "Source asset does not exist: " + entry.from.string();
            return result;
        }
        if (fs::exists(entry.to)) {
            result.error = "Destination already exists: " + entry.to.string();
            return result;
        }
    }

    struct Rewrite { fs::path path; std::string before; std::string after; };
    std::vector<Rewrite> rewrites;
    if (!copying) {
        std::error_code scanError;
        for (fs::recursive_directory_iterator it(root, scanError), end;
             !scanError && it != end; it.increment(scanError)) {
            if (!it->is_regular_file(scanError)) continue;
            const auto size = it->file_size(scanError);
            if (scanError || !isReferenceText(it->path(), size)) {
                scanError.clear();
                continue;
            }
            std::error_code ioError;
            std::string before = readFile(it->path(), ioError);
            if (ioError) continue;
            std::string after = before;
            std::size_t replacements = 0;
            for (const MoveEntry& entry : entries) {
                replacements += replaceAll(after, entry.oldLogical,
                                           entry.newLogical);
                replacements += replaceAll(after, entry.oldPortable,
                                           entry.newPortable);
                replacements += replaceAll(after, slashes(entry.from.string()),
                                           slashes(entry.to.string()));
            }
            if (after != before) {
                fs::path target = it->path();
                for (const MoveEntry& entry : entries) {
                    if (target == entry.from) { target = entry.to; break; }
                }
                rewrites.push_back({target, std::move(before), std::move(after)});
                result.updatedReferences += replacements;
            }
        }
        if (scanError) {
            result.error = "Reference scan failed: " + scanError.message();
            return result;
        }
    }

    std::vector<MoveEntry> completed;
    for (const MoveEntry& entry : entries) {
        std::error_code error;
        fs::create_directories(entry.to.parent_path(), error);
        if (!error) {
            if (copying) {
                fs::copy_file(entry.from, entry.to,
                              fs::copy_options::none, error);
            } else {
                fs::rename(entry.from, entry.to, error);
                if (error) {
                    error.clear();
                    fs::copy_file(entry.from, entry.to,
                                  fs::copy_options::none, error);
                    if (!error) fs::remove(entry.from, error);
                }
            }
        }
        if (error) {
            result.error = entry.oldLogical + ": " + error.message();
            for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
                std::error_code ignored;
                if (copying) fs::remove(it->to, ignored);
                else fs::rename(it->to, it->from, ignored);
            }
            return result;
        }
        completed.push_back(entry);
    }

    std::vector<Rewrite*> written;
    for (Rewrite& rewrite : rewrites) {
        std::error_code error;
        if (!writeFile(rewrite.path, rewrite.after, error)) {
            result.error = "Could not update references in "
                + rewrite.path.string() + ": " + error.message();
            std::error_code ignored;
            (void)writeFile(rewrite.path, rewrite.before, ignored);
            for (Rewrite* previous : written) {
                (void)writeFile(previous->path, previous->before, ignored);
            }
            for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
                if (copying) fs::remove(it->to, ignored);
                else fs::rename(it->to, it->from, ignored);
            }
            return result;
        }
        written.push_back(&rewrite);
    }

    result.affectedAssets = entries.size();
    for (const MoveEntry& entry : entries) {
        result.resultingLogicalPaths.push_back(entry.newLogical);
    }
    return result;
}

fs::path absoluteFolder(const fs::path& projectRoot,
                        const std::string& logicalFolder)
{
    const std::string normalized = slashes(logicalFolder);
    if (normalized == "Assets" || normalized.rfind("Assets/", 0) == 0) {
        return projectRoot / normalized;
    }
    if (normalized == "Imported" || normalized.rfind("Imported/", 0) == 0) {
        const std::string tail = normalized.size() > 8
            ? normalized.substr(9) : std::string{};
        return projectRoot / ".ayeditor_cache" / "assets" / tail;
    }
    return {};
}

} // namespace

EditorAssetOperations::EditorAssetOperations(std::string projectRoot)
{
    setProjectRoot(std::move(projectRoot));
}

void EditorAssetOperations::setProjectRoot(std::string projectRoot)
{
    std::error_code error;
    const fs::path root = projectRoot.empty()
        ? fs::path{} : fs::absolute(projectRoot, error).lexically_normal();
    _projectRoot = error ? std::string{} : root.string();
}

EditorAssetOperationResult EditorAssetOperations::rename(
    const EditorAssetDatabase& database, const EditorAssetRecord& record,
    const std::string& newFileName) const
{
    if (!validFileName(newFileName)) {
        return {0u, 0u, {}, "The new file name is invalid."};
    }
    const fs::path from(record.absolutePath);
    const fs::path to = from.parent_path() / newFileName;
    const std::string parent = fs::path(record.logicalPath).parent_path()
        .generic_string();
    const std::string newLogical = parent.empty()
        ? newFileName : parent + "/" + newFileName;
    MoveEntry entry{from, to, record.logicalPath, newLogical,
                    database.portableAssetPath(record),
                    database.portableAssetPath(newLogical)};
    return relocate(_projectRoot, database, {std::move(entry)}, false);
}

EditorAssetOperationResult EditorAssetOperations::move(
    const EditorAssetDatabase& database,
    const std::vector<EditorAssetRecord>& records,
    const std::string& destinationLogicalFolder) const
{
    const fs::path folder = absoluteFolder(_projectRoot,
        destinationLogicalFolder).lexically_normal();
    if (folder.empty()) return {0u, 0u, {}, "Destination must be under Assets or Imported."};
    std::vector<MoveEntry> entries;
    for (const EditorAssetRecord& record : records) {
        const std::string logical = slashes(destinationLogicalFolder)
            + "/" + fs::path(record.logicalPath).filename().generic_string();
        entries.push_back({record.absolutePath,
            folder / fs::path(record.absolutePath).filename(),
            record.logicalPath, logical, database.portableAssetPath(record),
            database.portableAssetPath(logical)});
    }
    return relocate(_projectRoot, database, std::move(entries), false);
}

EditorAssetOperationResult EditorAssetOperations::copy(
    const EditorAssetDatabase& database,
    const std::vector<EditorAssetRecord>& records,
    const std::string& destinationLogicalFolder) const
{
    const fs::path folder = absoluteFolder(_projectRoot,
        destinationLogicalFolder).lexically_normal();
    if (folder.empty()) return {0u, 0u, {}, "Destination must be under Assets or Imported."};
    std::vector<MoveEntry> entries;
    for (const EditorAssetRecord& record : records) {
        const std::string logical = slashes(destinationLogicalFolder)
            + "/" + fs::path(record.logicalPath).filename().generic_string();
        entries.push_back({record.absolutePath,
            folder / fs::path(record.absolutePath).filename(),
            record.logicalPath, logical, database.portableAssetPath(record),
            database.portableAssetPath(logical)});
    }
    return relocate(_projectRoot, database, std::move(entries), true);
}

} // namespace ayt::editor
