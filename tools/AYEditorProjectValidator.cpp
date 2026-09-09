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
    if (argc != 2 && argc != 4) {
        std::cerr << "Usage: AYEditorProjectValidator <project-root> "
                     "[--profile headless|full-client|all]\n";
        return 2;
    }
    std::string requestedProfile = "all";
    if (argc == 4) {
        if (std::string(argv[2]) != "--profile") {
            std::cerr << "Expected --profile before the profile name.\n";
            return 2;
        }
        requestedProfile = argv[3];
        if (requestedProfile != "headless"
            && requestedProfile != "full-client"
            && requestedProfile != "all") {
            std::cerr << "Unknown validation profile: "
                      << requestedProfile << '\n';
            return 2;
        }
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

    const bool headless = requestedProfile == "full-client" || validate(
        projectRoot, ayt::editor::EditorRuntimeValidationProfile::Headless);
    const bool fullClient = requestedProfile == "headless" || validate(
        projectRoot, ayt::editor::EditorRuntimeValidationProfile::FullClient);
    return headless && fullClient ? 0 : 1;
}
