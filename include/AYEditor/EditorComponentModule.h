#pragma once

#include <AYModule/IModule.h>

#include <string_view>

namespace ayt::editor
{

inline constexpr std::string_view kEditorComponentModuleId =
    "AYEditor.Components";

class EditorComponentModule final : public ayt::module::IModule
{
public:
    EditorComponentModule();

    [[nodiscard]] const ayt::module::ModuleDescriptor& descriptor()
        const noexcept override;
    [[nodiscard]] ayt::module::ModuleResult registerTypes(
        ayt::module::IModuleContext& context) override;

private:
    ayt::module::ModuleDescriptor _descriptor;
};

} // namespace ayt::editor
