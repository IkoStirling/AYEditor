#include <AYEditor/EditorComponentModule.h>

#include <AYEditor/EditorPlayerController.h>
#include <AYEntity/EntityComponentModule.h>

#include <string>

namespace ayt::editor
{

EditorComponentModule::EditorComponentModule()
    : _descriptor{
          .id = std::string(kEditorComponentModuleId),
          .displayName = "AYEditor Components",
          .version = "0.1.0",
          .dependencies = {
              ayt::module::ModuleDependency::required(
                  std::string(ayt::entity::kEntityComponentModuleId))}}
{
}

const ayt::module::ModuleDescriptor& EditorComponentModule::descriptor()
    const noexcept
{
    return _descriptor;
}

ayt::module::ModuleResult EditorComponentModule::registerTypes(
    ayt::module::IModuleContext& context)
{
    auto* registry = context.findServiceAs<ayt::entity::ComponentRegistry>(
        ayt::entity::kComponentRegistryModuleService);
    if (registry == nullptr) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::TypeRegistrationFailed,
            "ComponentRegistry service is unavailable for AYEditor.Components");
    }

    ayt::entity::ComponentRegistryResult result =
        registerEditorPlayerControllerComponent(*registry);
    if (!result) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::TypeRegistrationFailed,
            result.message());
    }
    return ayt::module::ModuleResult::success();
}

} // namespace ayt::editor
