#include "EditorPlayerController.h"

#include <AYEntityModule.h>
#include <AYWorld.h>

#include <ayreflect/IReflect.h>
#include <AYReflect.h>
#include <AYReflectMacros.h>

#include <logia/AYSemanticAnalyzer.h>

// INT-01 (2026-07-15): Register `PlayerController` with AYReflect under
// the bare name "PlayerController" so the codegen multi-hop rewrite
// for `self.position.x` resolves the host type. Mirrors
// Test_LogiaCodegen.cpp::codegen_full_player_controller's PCStub block
// (lines 197-244). The host type name baked into emitted Lua is
// "PlayerController" — must match `World::registerComponentType<T>`.
//
// Idempotent: the findType guard avoids duplicate registration; multiple
// Editor startups / world resets must not double-register the same
// ITypeInfo pointer into the AYReflect TypeRegistry (would corrupt the
// CRT debug heap per AYEntityReflection.cpp:3-5).

namespace ayt::editor {

namespace {

bool ensurePlayerControllerRegisteredOnce()
{
    auto& reg = ayt::reflect::TypeRegistryImpl::instance();
    if (reg.findType("PlayerController") != nullptr) {
        return true;
    }

    // S3.11: ensure FVector3 is registered with its x/y/z primitives
    // so the chain reflect rewrite has leaf-resolved types to stamp.
    // The SemanticAnalyzer ctor's side effect (`ensureAYEntityTypesRegistered`)
    // is the public hook for this — calling here means the Editor
    // spawn path doesn't have to wait for the first bridge compile.
    {
        ayt::script::logia::SemanticAnalyzer probe;
        (void)probe;
    }

    auto* fvec3 = reg.findType("FVector3");
    auto* floatInfo = reg.findType("float");

    auto* info = new ayt::reflect::TypeInfoImpl<PlayerController>(
        "PlayerController",
        ayt::reflect::detail::defaultCreate<PlayerController>,
        ayt::reflect::detail::defaultDestroy<PlayerController>,
        ayt::reflect::detail::defaultCopy<PlayerController>);

    if (fvec3) {
        info->addField(new ayt::reflect::FieldInfoImpl(
            "position", fvec3,
            offsetof(PlayerController, position),
            ayt::reflect::FieldAttribute::Serialize));
    }
    if (floatInfo) {
        info->addField(new ayt::reflect::FieldInfoImpl(
            "speed", floatInfo, offsetof(PlayerController, speed),
            ayt::reflect::FieldAttribute::Serialize));
        info->addField(new ayt::reflect::FieldInfoImpl(
            "jump_force", floatInfo, offsetof(PlayerController, jump_force),
            ayt::reflect::FieldAttribute::Serialize));
    }

    reg.registerTypeInfo("PlayerController", info);
    return true;
}

struct PlayerControllerRegistrar {
    PlayerControllerRegistrar() {
        if (!ensurePlayerControllerRegisteredOnce()) {
            return;
        }
        ayt::entity::World::registerComponentType<PlayerController>("PlayerController");
    }
};

static PlayerControllerRegistrar g_playerControllerRegistrar;

} // namespace

} // namespace ayt::editor
