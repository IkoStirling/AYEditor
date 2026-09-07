#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYEditor/RegisterDefaultEditorModules.h>
#include <AYEditor/EditorPlayerController.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYGameLoop/SubSystemRegistry.h>
#include <AYModule/ModuleTypes.h>
#include <AYTest.h>

#include <vector>

namespace ayt::editor::test
{

TEST_SUITE(DefaultEditorModuleAssemblyTests)

TEST_CASE(editor_graph_installs_all_default_gameloop_subsystems)
{
    auto& registry = ayt::game::SubSystemRegistry::instance();
    registry.clearAll();

    ayt::app::EngineModuleRuntime runtime(ayt::app::defaultEngineHost());
    EditorModuleOptions options{};
    options.enableAudio = false;

    CHECK_TRUE(configureDefaultEditorModules(runtime, options).succeeded());
    CHECK_TRUE(runtime.prepare().succeeded());
    runtime.context().componentRegistry().seal();

    const std::vector<ayt::module::ModuleId> expectedOrder{
        "AYEntity.Components",
        "AYEditor.Components",
        "AYEntity.Runtime",
        "AYRenderer.Runtime",
        "AYEntity.AnimationIntegration",
        "AYEntity.RenderIntegration",
        "AYEntity.2DIntegration",
        "AYPhysics.Runtime",
        "AYEntity.PhysicsIntegration",
        "AYEntity.ScriptIntegration",
        "AYScript.Runtime",
        "AYEntity.NetworkIntegration",
        "AYNetwork.Runtime"};
    CHECK(runtime.modules().orderedModuleIds() == expectedOrder);
    CHECK_NOT_NULL(runtime.context().componentRegistry().find<PlayerController>());

    CHECK_TRUE(runtime.install().succeeded());
    CHECK_NOT_NULL(registry.findSubSystem("Entity"));
    CHECK_NOT_NULL(registry.findSubSystem("Renderer"));
    CHECK_NOT_NULL(registry.findSubSystem("Physics"));
    CHECK_NOT_NULL(registry.findSubSystem("EntityPhysicsBridge"));
    CHECK_NOT_NULL(registry.findSubSystem("ayt.script.runtime"));
    CHECK_NOT_NULL(registry.findSubSystem("Network"));
    CHECK(registry.findSubSystem("Audio") == nullptr);

    runtime.shutdown();
    CHECK(registry.getCount() == 0);
}

TEST_SUITE_END

} // namespace ayt::editor::test
