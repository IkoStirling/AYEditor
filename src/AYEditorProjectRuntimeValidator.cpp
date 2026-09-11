#include "AYEditor/EditorProjectRuntimeValidator.h"

#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorProjectUiFlow.h"

#include <AYApplication/ProjectContentValidator.h>
#include <AYApplication/UIFlowAssetValidation.h>

#include <AYUI/UIFlow.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{

void validateUiFlow(
    const std::filesystem::path& projectRoot,
    const ayt::editor::EditorProjectDescriptor& descriptor,
    ayt::editor::EditorRuntimeValidationProfile profile,
    ayt::editor::EditorRuntimeValidationResult& result)
{
    ayt::editor::EditorProjectUiFlowResolution resolution;
    std::string error;
    if (!ayt::editor::resolveEditorProjectUiFlow(
            descriptor, resolution, &error)) {
        result.issues.push_back({descriptor.sourcePath, std::move(error)});
        return;
    }
    if (!resolution) return;

    ayt::ui::UIFlowDocument flow;
    std::string flowAsset = "<legacy-world-ui>";
    std::filesystem::path flowPath(descriptor.sourcePath);
    if (resolution.source
        == ayt::editor::EditorProjectUiFlowSource::LegacyWorldLayouts) {
        flow = resolution.compatibilityFlow;
    } else {
        flowAsset = resolution.flowAsset;
        flowPath = projectRoot / descriptor.assetRoot / resolution.flowAsset;
        std::ifstream input(flowPath, std::ios::binary);
        if (!input) {
            result.issues.push_back({flowPath.string(),
                "Project UI Flow asset does not exist."});
            return;
        }
        ++result.uiFlows;
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
        if (!ayt::ui::UIFlowSerializer::deserialize(
                text, flow, &diagnostics)) {
            for (const auto& diagnostic : diagnostics) {
                result.issues.push_back({flowPath.string()
                    + (diagnostic.path.empty() ? std::string{}
                                               : " " + diagnostic.path),
                    diagnostic.message});
            }
            if (diagnostics.empty()) {
                result.issues.push_back({flowPath.string(),
                    "UI Flow serializer rejected the asset."});
            }
            return;
        }
    }

    const auto addReferenceError = [&](std::string path,
                                       std::string message) {
        result.issues.push_back({descriptor.sourcePath + " " + std::move(path),
                                 std::move(message)});
    };
    if (!resolution.entry.empty()
        && flow.findEntry(resolution.entry) == nullptr) {
        addReferenceError("$.ui.entry",
            "Project ui.entry references an unknown Flow Entry '"
                + resolution.entry + "'.");
    }
    for (std::size_t index = 0; index < resolution.worldContexts.size(); ++index) {
        const auto& binding = resolution.worldContexts[index];
        if (flow.findContext(binding.contextId) == nullptr) {
            const auto world = std::find_if(
                descriptor.worlds.begin(), descriptor.worlds.end(),
                [&](const ayt::editor::EditorProjectWorldDescriptor& value) {
                    return value.id == binding.worldId;
                });
            const std::size_t worldIndex = world == descriptor.worlds.end()
                ? index : static_cast<std::size_t>(
                    std::distance(descriptor.worlds.begin(), world));
            addReferenceError("$.worlds[" + std::to_string(worldIndex)
                    + "].uiContext",
                "World '" + binding.worldId
                    + "' references an unknown UI Context '"
                    + binding.contextId + "'.");
        }
    }

    const auto validation = ayt::app::validateUIFlowAssets(
        flow, (projectRoot / descriptor.assetRoot).string(),
        profile == ayt::editor::EditorRuntimeValidationProfile::FullClient
            ? ayt::app::UIFlowAssetValidationProfile::FullClient
            : ayt::app::UIFlowAssetValidationProfile::StructureOnly);
    for (const auto& diagnostic : validation.diagnostics) {
        if (diagnostic.severity
            != ayt::ui::UIFlowDiagnosticSeverity::Error) continue;
        result.issues.push_back({flowPath.string()
            + (diagnostic.path.empty() ? std::string{}
                                       : " " + diagnostic.path),
            diagnostic.message});
    }
    for (const auto& dependency : validation.dependencies) {
        result.uiFlowDependencies.push_back({
            flowAsset, dependency.asset, dependency.screens});
    }
}

} // namespace

namespace ayt::editor {

EditorRuntimeValidationResult EditorProjectRuntimeValidator::validate(
    const std::string& projectRoot, EditorRuntimeValidationProfile profile)
{
    const ayt::app::ProjectContentValidationProfile applicationProfile =
        profile == EditorRuntimeValidationProfile::Headless
        ? ayt::app::ProjectContentValidationProfile::Headless
        : ayt::app::ProjectContentValidationProfile::FullClient;
    const ayt::app::ProjectContentValidationResult validated =
        ayt::app::validateProjectContent(projectRoot, applicationProfile);
    EditorRuntimeValidationResult result;
    result.profile = profile;
    result.scenes = validated.scenes;
    result.uiLayouts = validated.uiLayouts;
    result.tilemaps = validated.tilemaps;
    result.issues.reserve(validated.issues.size());
    for (const ayt::app::ProjectContentValidationIssue& issue :
         validated.issues) {
        result.issues.push_back({issue.path, issue.message});
    }
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(projectRoot);
    if (descriptor) {
        validateUiFlow(std::filesystem::absolute(projectRoot).lexically_normal(),
                       descriptor, profile, result);
    }
    return result;
}

} // namespace ayt::editor
