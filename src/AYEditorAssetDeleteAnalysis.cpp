#include "AYEditor/EditorAssetDeleteAnalysis.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

namespace ayt::editor {
namespace {

std::string normalized(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool textAuthoringAsset(const EditorAssetRecord& record)
{
    if (record.type == EditorAssetType::Scene
        || record.type == EditorAssetType::Material
        || record.type == EditorAssetType::Script
        || record.type == EditorAssetType::Shader
        || record.type == EditorAssetType::UiLayout) {
        return true;
    }
    const std::string extension = lower(
        std::filesystem::path(record.absolutePath).extension().string());
    return extension == ".json" || extension == ".toml"
        || extension == ".yaml" || extension == ".yml";
}

} // namespace

EditorAssetDeleteAnalysis analyzeEditorAssetDeletion(
    const EditorAssetDatabase& database,
    const std::vector<EditorAssetId>& selectedIds)
{
    EditorAssetDeleteAnalysis analysis;
    std::unordered_set<EditorAssetId> selected(
        selectedIds.begin(), selectedIds.end());
    std::vector<const EditorAssetRecord*> targets;
    for (EditorAssetId id : selectedIds) {
        if (const EditorAssetRecord* record = database.find(id)) {
            targets.push_back(record);
        }
    }

    constexpr std::uintmax_t maxTextAssetSize = 4u * 1024u * 1024u;
    for (const EditorAssetRecord& referring : database.records()) {
        if (selected.count(referring.id) != 0u
            || !textAuthoringAsset(referring)
            || referring.size > maxTextAssetSize) {
            continue;
        }
        std::ifstream input(referring.absolutePath,
                            std::ios::binary | std::ios::ate);
        if (!input) continue;
        const std::streamoff length = input.tellg();
        if (length < 0 || static_cast<std::uintmax_t>(length)
                > maxTextAssetSize) {
            continue;
        }
        input.seekg(0);
        std::string contents((std::istreambuf_iterator<char>(input)), {});
        contents = normalized(std::move(contents));

        for (const EditorAssetRecord* target : targets) {
            std::vector<std::string> candidates = {
                normalized(target->logicalPath),
                normalized(database.portableAssetPath(*target)),
                normalized(target->runtimePath),
                normalized(target->name),
            };
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [](const std::string& candidate) { return candidate.empty(); }),
                candidates.end());
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
            if (std::any_of(candidates.begin(), candidates.end(),
                    [&contents](const std::string& candidate) {
                        return contents.find(candidate) != std::string::npos;
                    })) {
                analysis.references.push_back({
                    target->id, target->logicalPath,
                    referring.id, referring.logicalPath});
            }
        }
    }
    std::sort(analysis.references.begin(), analysis.references.end(),
        [](const EditorAssetReference& left,
           const EditorAssetReference& right) {
            if (left.targetPath != right.targetPath) {
                return left.targetPath < right.targetPath;
            }
            return left.referencingPath < right.referencingPath;
        });
    return analysis;
}

} // namespace ayt::editor
