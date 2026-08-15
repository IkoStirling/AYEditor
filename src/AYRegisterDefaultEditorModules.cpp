#include "AYRegisterDefaultEditorModules.h"

#include "AYScriptSubSystem.h"

#include <AYAudioBackendFactory.h>
#include <AYAudioSubSystem.h>
#include <AYEntityModule.h>
#include <AYGameLoop.h>
#include <AYNetworkModule.h>
#include <AYRegisterDefaultModules.h>

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

} // namespace ayt::editor
