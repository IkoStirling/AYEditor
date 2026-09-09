#include "AYEditor/EditorProjectRuntimeValidator.h"

#include <AYApplication/ProjectContentValidator.h>

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
    return result;
}

} // namespace ayt::editor
