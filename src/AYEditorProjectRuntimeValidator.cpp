#include "AYEditor/EditorProjectRuntimeValidator.h"

#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorProjectUiFlow.h"

#include <AYApplication/ProjectContentValidator.h>
#include <AYApplication/UIFlowAssetValidation.h>

#include <AYUI/UIFlow.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace
{

namespace fs = std::filesystem;

bool hasWindowsDrivePrefix(std::string_view value) noexcept
{
    if (value.size() < 2 || value[1] != ':') return false;
    const char letter = value.front();
    return (letter >= 'A' && letter <= 'Z')
        || (letter >= 'a' && letter <= 'z');
}

bool pathComponentEqual(const fs::path& left, const fs::path& right)
{
#ifdef _WIN32
    const std::string leftText = left.string();
    const std::string rightText = right.string();
    return leftText.size() == rightText.size()
        && std::equal(leftText.begin(), leftText.end(), rightText.begin(),
            [](char lhs, char rhs) {
                return std::tolower(static_cast<unsigned char>(lhs))
                    == std::tolower(static_cast<unsigned char>(rhs));
            });
#else
    return left == right;
#endif
}

bool isWithin(const fs::path& root, const fs::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end()
            || !pathComponentEqual(*rootIt, *candidateIt)) return false;
    }
    return true;
}

bool resolveContainedPath(const fs::path& base,
                          std::string_view relative,
                          fs::path& result,
                          std::string& error)
{
    if (hasWindowsDrivePrefix(relative)) {
        error = "Path must be relative to its content root: "
            + std::string(relative);
        return false;
    }
    if (relative.find('\\') != std::string_view::npos) {
        error = "Path must use portable forward-slash separators: "
            + std::string(relative);
        return false;
    }
    const fs::path supplied(relative);
    if (supplied.empty() || supplied.is_absolute()
        || supplied.has_root_name() || supplied.has_root_directory()) {
        error = "Path must be relative to its content root: "
            + std::string(relative);
        return false;
    }
    for (const auto& part : supplied) {
        if (part == "..") {
            error = "Path must stay inside its content root: "
                + std::string(relative);
            return false;
        }
    }

    std::error_code filesystemError;
    const fs::path canonicalBase = fs::canonical(base, filesystemError);
    if (filesystemError) {
        error = "Content root cannot be resolved: " + base.string()
            + ": " + filesystemError.message();
        return false;
    }

    const fs::path candidate = (canonicalBase / supplied).lexically_normal();
    if (!isWithin(canonicalBase, candidate)) {
        error = "Path resolves outside its content root: "
            + std::string(relative);
        return false;
    }

    // canonical()/weakly_canonical() can report access_denied for a missing
    // leaf on some Windows filesystems. Resolve the nearest existing ancestor
    // instead, then append the still-missing suffix. Existing symlink parents
    // are canonicalized and cannot be used to escape the content root.
    fs::path probe = candidate;
    fs::path missingSuffix;
    while (true) {
        filesystemError.clear();
        const bool exists = fs::exists(probe, filesystemError);
        if (filesystemError) {
            error = "Path cannot be inspected: " + std::string(relative)
                + ": " + filesystemError.message();
            return false;
        }
        if (exists) break;
        if (probe.empty() || probe == probe.parent_path()) {
            error = "Path cannot be resolved: " + std::string(relative);
            return false;
        }
        missingSuffix = probe.filename() / missingSuffix;
        probe = probe.parent_path();
    }

    const fs::path resolvedPrefix = fs::canonical(probe, filesystemError);
    if (filesystemError) {
        error = "Path cannot be resolved: " + std::string(relative)
            + ": " + filesystemError.message();
        return false;
    }
    const fs::path resolved = missingSuffix.empty()
        ? resolvedPrefix.lexically_normal()
        : (resolvedPrefix / missingSuffix).lexically_normal();
    if (!isWithin(canonicalBase, resolved)) {
        error = "Path resolves outside its content root: "
            + std::string(relative);
        return false;
    }
    result = resolved;
    error.clear();
    return true;
}

void validateUiFlow(
    const fs::path& projectRoot,
    const ayt::editor::EditorProjectDescriptor& descriptor,
    ayt::editor::EditorRuntimeValidationProfile profile,
    ayt::editor::EditorRuntimeValidationResult& result)
{
    fs::path assetRoot;
    std::string error;
    if (!resolveContainedPath(
            projectRoot, descriptor.assetRoot, assetRoot, error)) {
        result.issues.push_back({descriptor.sourcePath, std::move(error)});
        return;
    }
    std::error_code assetRootError;
    if (!fs::is_directory(assetRoot, assetRootError)) {
        result.issues.push_back({assetRoot.string(), assetRootError
            ? "Project asset root cannot be inspected: "
                + assetRootError.message()
            : "Project asset root does not exist."});
        return;
    }

    ayt::editor::EditorProjectUiFlowResolution resolution;
    if (!ayt::editor::resolveEditorProjectUiFlow(
            descriptor, resolution, &error)) {
        result.issues.push_back({descriptor.sourcePath, std::move(error)});
        return;
    }
    if (!resolution) return;

    ayt::ui::UIFlowDocument flow;
    std::string flowAsset = "<legacy-world-ui>";
    fs::path flowPath(descriptor.sourcePath);
    if (resolution.source
        == ayt::editor::EditorProjectUiFlowSource::LegacyWorldLayouts) {
        flow = resolution.compatibilityFlow;
    } else {
        flowAsset = resolution.flowAsset;
        if (!resolveContainedPath(
                assetRoot, resolution.flowAsset, flowPath, error)) {
            result.issues.push_back({descriptor.sourcePath, std::move(error)});
            return;
        }
        std::error_code flowStatusError;
        const bool regular = fs::is_regular_file(flowPath, flowStatusError);
        if (flowStatusError || !regular) {
            result.issues.push_back({flowPath.string(), flowStatusError
                ? "Project UI Flow asset cannot be inspected: "
                    + flowStatusError.message()
                : "Project UI Flow asset must be a regular file."});
            return;
        }
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
        flow, assetRoot.string(),
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
    EditorRuntimeValidationResult result;
    result.profile = profile;
    std::error_code filesystemError;
    fs::path root = projectRoot.empty()
        ? fs::current_path(filesystemError) : fs::path(projectRoot);
    if (!filesystemError) root = fs::absolute(root, filesystemError);
    if (!filesystemError) root = fs::weakly_canonical(root, filesystemError);
    if (filesystemError) {
        result.issues.push_back({projectRoot,
            "Project root cannot be resolved: " + filesystemError.message()});
        return result;
    }

    fs::path descriptorPath;
    std::string descriptorError;
    if (!resolveContainedPath(root, kEditorProjectDescriptorFile,
            descriptorPath, descriptorError)) {
        result.issues.push_back({projectRoot, std::move(descriptorError)});
        return result;
    }
    const bool descriptorExists = fs::exists(
        descriptorPath, filesystemError);
    if (filesystemError) {
        result.issues.push_back({descriptorPath.string(),
            "Project descriptor cannot be inspected: "
                + filesystemError.message()});
        return result;
    }
    if (descriptorExists
        && !fs::is_regular_file(descriptorPath, filesystemError)) {
        result.issues.push_back({descriptorPath.string(), filesystemError
            ? "Project descriptor cannot be inspected: "
                + filesystemError.message()
            : "Project descriptor must be a regular file."});
        return result;
    }

    const ayt::app::ProjectContentValidationProfile applicationProfile =
        profile == EditorRuntimeValidationProfile::Headless
        ? ayt::app::ProjectContentValidationProfile::Headless
        : ayt::app::ProjectContentValidationProfile::FullClient;
    ayt::app::ProjectContentValidationOptions applicationOptions;
    applicationOptions.enableGameFlowUIActions = true;
    ayt::app::ProjectContentValidationResult validated;
    try {
        validated = ayt::app::validateProjectContent(
            root.string(), applicationProfile, std::move(applicationOptions));
    } catch (const std::exception& exception) {
        result.issues.push_back({projectRoot,
            std::string("Project content validation failed: ")
                + exception.what()});
    } catch (...) {
        result.issues.push_back({projectRoot,
            "Project content validation failed."});
    }
    result.scenes = validated.scenes;
    result.uiLayouts = validated.uiLayouts;
    result.tilemaps = validated.tilemaps;
    result.gameFlows = validated.gameFlows;
    result.issues.reserve(validated.issues.size());
    for (const ayt::app::ProjectContentValidationIssue& issue :
         validated.issues) {
        result.issues.push_back({issue.path, issue.message});
    }
    result.gameFlowDependencies.reserve(
        validated.gameFlowDependencies.size());
    for (const auto& dependency : validated.gameFlowDependencies) {
        result.gameFlowDependencies.push_back({
            ayt::app::projectContentDependencyKindName(dependency.kind),
            dependency.source, dependency.target});
    }
    if (!descriptorExists) return result;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(root.string(), &descriptorError);
    if (!descriptor) {
        if (descriptorExists) {
            const bool alreadyReported = std::any_of(
                result.issues.begin(), result.issues.end(),
                [&](const EditorRuntimeValidationIssue& issue) {
                    return fs::path(issue.path).lexically_normal()
                        == descriptorPath.lexically_normal();
                });
            if (!alreadyReported) {
                result.issues.push_back({descriptorPath.string(),
                    descriptorError.empty()
                        ? "Project descriptor is invalid."
                        : std::move(descriptorError)});
            }
        }
        return result;
    }
    validateUiFlow(root, descriptor, profile, result);
    return result;
}

} // namespace ayt::editor
