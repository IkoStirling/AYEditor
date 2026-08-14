#include "AYRegisterDefaultEditorModules.h"

#include "AYScriptSubSystem.h"

#include <AYEntityModule.h>
#include <AYGameLoop.h>
#include <AYNetworkModule.h>
#include <AYRegisterDefaultModules.h>

namespace ayt::editor
{

void registerDefaultEditorModules()
{
    // P1: shared presentation stack with Client (enablePresentation=true).
    ayt::app::registerEntityPresentationStack();
    ayt::net::registerNetworkSubSystem();

    ayt::game::GameLoop::instance().registerSubSystem(
        new ayt::script::ScriptSubSystem());
}

} // namespace ayt::editor
