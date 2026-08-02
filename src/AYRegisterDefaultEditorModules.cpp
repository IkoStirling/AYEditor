#include "AYRegisterDefaultEditorModules.h"

#include "AYRendererSubSystem.h"
#include "AYScriptSubSystem.h"

#include <AYEntityModule.h>
#include <AYGameLoop.h>
#include <AYNetworkModule.h>

namespace ayt::editor
{

void registerDefaultEditorModules()
{
    ayt::entity::bootstrapModule();
    ayt::net::registerNetworkSubSystem();
    ayt::render::RendererSubSystem::registerSubSystem();

    ayt::game::GameLoop::instance().registerSubSystem(
        new ayt::script::ScriptSubSystem());
}

} // namespace ayt::editor
