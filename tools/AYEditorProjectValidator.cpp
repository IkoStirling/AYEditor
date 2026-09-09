#include <AYEditor/EditorProjectDescriptor.h>
#include <AYEditor/EditorProjectRuntimeValidator.h>
#include <AYEntity/EntityModule.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

const char* profileName(ayt::editor::EditorRuntimeValidationProfile profile)
{
    return profile == ayt::editor::EditorRuntimeValidationProfile::Headless
        ? "headless" : "full-client";
}

bool validate(const std::filesystem::path& projectRoot,
              ayt::editor::EditorRuntimeValidationProfile profile)
{
    const auto result = ayt::editor::EditorProjectRuntimeValidator::validate(
        projectRoot.string(), profile);
    std::cout << profileName(profile) << ": " << result.checked()
              << " file(s): " << result.scenes << " scene(s), "
              << result.uiLayouts << " UI layout(s), " << result.tilemaps
              << " tilemap file(s)" << '\n';
    for (const auto& issue : result.issues) {
        std::cerr << "  " << issue.path << ": " << issue.message << '\n';
    }
    return static_cast<bool>(result);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: AYEditorProjectValidator <project-root>\n";
        return 2;
    }

    const std::filesystem::path projectRoot =
        std::filesystem::absolute(argv[1]).lexically_normal();
    ayt::entity::registerEntityComponents();
    std::string descriptorError;
    const auto descriptor = ayt::editor::EditorProjectDescriptor::load(
        projectRoot.string(), &descriptorError);
    if (!descriptor) {
        std::cerr << descriptorError << '\n';
        return 1;
    }

    std::cout << descriptor.displayName << " (" << descriptor.id << ")\n"
              << "assets: " << descriptor.assetRoot << '\n'
              << "startup World: " << descriptor.startupWorld << '\n';

    const bool headless = validate(projectRoot,
        ayt::editor::EditorRuntimeValidationProfile::Headless);
    const bool fullClient = validate(projectRoot,
        ayt::editor::EditorRuntimeValidationProfile::FullClient);
    return headless && fullClient ? 0 : 1;
}
