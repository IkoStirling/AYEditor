#include "AYEditor/RegisterDefaultEditorModules.h"

#include <AYApplication/EngineModuleRuntime.h>
#include <AYAudio/AudioRuntimeModule.h>
#include "AYScript/ScriptSubSystem.h"

#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/AudioSubSystem.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYEntity/Entity2DIntegrationModule.h>
#include <AYEntity/EntityAnimationIntegrationModule.h>
#include <AYEntity/EntityComponentModule.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/EntityNetworkIntegrationModule.h>
#include <AYEntity/EntityPhysicsIntegrationModule.h>
#include <AYEntity/EntityRenderIntegrationModule.h>
#include <AYEntity/EntityRuntimeModule.h>
#include <AYEntity/EntityScriptIntegrationModule.h>
#include <AYEditor/EditorComponentModule.h>
#include <AYGameLoop.h>
#include <AYNetwork/NetworkModule.h>
#include <AYNetwork/NetworkRuntimeModule.h>
#include <AYApplication/RegisterDefaultModules.h>
#include <AYPhysics/PhysicsRuntimeModule.h>
#include <AYRenderer/RendererRuntimeModule.h>
#include <AYScript/ScriptRuntimeModule.h>

#include <memory>

namespace ayt::editor
{

void registerDefaultEditorModules()
{
    registerDefaultEditorModules(EditorModuleOptions{});
}

void registerDefaultEditorModules(const EditorModuleOptions& options)
{
    // P1: shared presentation stack with Client (enablePresentation=true).
    ayt::app::registerEntityPresentationStack();
    ayt::app::registerPhysicsModule();
    ayt::net::registerNetworkSubSystem();

    ayt::game::GameLoop::instance().registerSubSystem(
        new ayt::script::ScriptSubSystem());

    if (!options.enableAudio) {
        return;
    }

    auto audioSub = std::make_unique<ayt::audio::AudioSubSystem>();
    audioSub->setBackend(ayt::audio::makeMiniaudioBackend());
    ayt::game::GameLoop::instance().registerSubSystem(audioSub.release());
}

ayt::module::ModuleResult configureDefaultEditorModules(
    ayt::app::EngineModuleRuntime& runtime,
    const EditorModuleOptions& options)
{
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityComponentModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            EditorComponentModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityRuntimeModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::render::RendererRuntimeModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityAnimationIntegrationModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityRenderIntegrationModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::Entity2DIntegrationModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::physics::PhysicsRuntimeModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityPhysicsIntegrationModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityScriptIntegrationModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::script::ScriptRuntimeModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityNetworkIntegrationModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::net::NetworkRuntimeModule>(); !result) {
        return result;
    }
    if (options.enableAudio) {
        if (auto result = runtime.modules().emplace<
                ayt::audio::AudioRuntimeModule>(
                    []() { return ayt::audio::makeMiniaudioBackend(); });
            !result) {
            return result;
        }
    }
    return ayt::module::ModuleResult::success();
}

} // namespace ayt::editor
