#include "AYEditor/EditorComponentPolicy.h"

#include <AYEntity.h>
#include <AYEntity/ComponentRegistry.h>

#include <algorithm>
#include <unordered_set>

namespace ayt::editor {

EditorComponentPolicyRegistry& EditorComponentPolicyRegistry::instance()
{
    static EditorComponentPolicyRegistry registry;
    return registry;
}

void EditorComponentPolicyRegistry::installDefaults()
{
    if (_defaultsInstalled) return;
    _defaultsInstalled = true;

    const auto add = [this](const char* component,
                            std::initializer_list<const char*> prerequisites,
                            bool removable = true) {
        EditorComponentPolicy policy;
        policy.componentType = component;
        policy.removable = removable;
        for (const char* required : prerequisites) {
            policy.prerequisites.emplace_back(required);
        }
        (void)registerPolicy(std::move(policy));
    };

    add("Transform", {}, false);
    add("SimTransformComponent", {"Transform"});
    add("MeshComponent", {"Transform"});
    add("SpriteComponent", {"Transform"});
    add("TilemapComponent", {"Transform"});
    add("OrthoCameraComponent", {"Transform"});
    add("RigidBodyComponent", {"Transform"});
    add("ColliderComponent", {"Transform"});
    add("AudioEmitterComponent", {"Transform"});
    add("AudioListenerComponent", {"Transform"});
    add("SkeletonComponent", {"Transform"});
    add("AnimationComponent", {"SkeletonComponent"});
    add("BlendSpaceComponent", {"SkeletonComponent"});
    add("AnimationStateMachineComponent", {"SkeletonComponent"});
}

void EditorComponentPolicyRegistry::clear()
{
    _policies.clear();
    _defaultsInstalled = false;
}

bool EditorComponentPolicyRegistry::registerPolicy(
    EditorComponentPolicy policy, std::string* error)
{
    if (policy.componentType.empty()) {
        if (error) *error = "component policy has an empty type name";
        return false;
    }
    for (const std::string& required : policy.prerequisites) {
        if (required.empty() || required == policy.componentType) {
            if (error) *error = "component policy has an invalid prerequisite";
            return false;
        }
    }
    _policies[policy.componentType] = std::move(policy);
    return true;
}

const EditorComponentPolicy* EditorComponentPolicyRegistry::find(
    std::string_view componentType) const
{
    const auto found = _policies.find(std::string(componentType));
    return found == _policies.end() ? nullptr : &found->second;
}

bool EditorComponentPolicyRegistry::addWithRequirements(
    ayt::entity::Entity& entity, std::string_view componentType,
    std::vector<std::string>* added, std::string* error) const
{
    std::vector<std::string> localAdded;
    std::unordered_set<std::string> visiting;
    const auto addOne = [&](const auto& self, const std::string& typeName) -> bool {
        const auto* descriptor =
            ayt::entity::ComponentRegistry::instance().find(typeName);
        if (descriptor == nullptr || descriptor->has == nullptr
            || descriptor->add == nullptr || !descriptor->editorAddable) {
            if (error) *error = "component is unavailable for authoring: " + typeName;
            return false;
        }
        if (descriptor->has(entity)) return true;
        if (!visiting.insert(typeName).second) {
            if (error) *error = "component prerequisite cycle at " + typeName;
            return false;
        }
        if (const EditorComponentPolicy* policy = find(typeName)) {
            for (const std::string& required : policy->prerequisites) {
                if (!self(self, required)) {
                    visiting.erase(typeName);
                    return false;
                }
            }
        }
        visiting.erase(typeName);
        if (descriptor->add(entity) == nullptr) {
            if (error) *error = "failed to add component: " + typeName;
            return false;
        }
        localAdded.push_back(typeName);
        return true;
    };

    if (!addOne(addOne, std::string(componentType))) {
        for (auto it = localAdded.rbegin(); it != localAdded.rend(); ++it) {
            const auto* descriptor =
                ayt::entity::ComponentRegistry::instance().find(*it);
            if (descriptor != nullptr && descriptor->remove != nullptr
                && descriptor->has != nullptr && descriptor->has(entity)) {
                descriptor->remove(entity);
            }
        }
        return false;
    }
    if (added) *added = std::move(localAdded);
    return true;
}

bool EditorComponentPolicyRegistry::canRemove(
    const ayt::entity::Entity& entity, std::string_view componentType,
    std::string* reason) const
{
    const std::string typeName(componentType);
    if (const EditorComponentPolicy* own = find(typeName);
        own != nullptr && !own->removable) {
        if (reason) *reason = typeName + " is required by the entity";
        return false;
    }
    for (const auto& [dependentName, policy] : _policies) {
        if (std::find(policy.prerequisites.begin(),
                      policy.prerequisites.end(), typeName)
            == policy.prerequisites.end()) {
            continue;
        }
        const auto* dependent =
            ayt::entity::ComponentRegistry::instance().find(dependentName);
        if (dependent != nullptr && dependent->has != nullptr
            && dependent->has(entity)) {
            if (reason) {
                *reason = typeName + " is required by "
                    + (dependent->displayName.empty()
                           ? dependentName : dependent->displayName);
            }
            return false;
        }
    }
    if (reason) reason->clear();
    return true;
}

} // namespace ayt::editor
