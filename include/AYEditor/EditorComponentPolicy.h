#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ayt::entity { class Entity; }

namespace ayt::editor {

// Editor-only composition rules. Runtime component descriptors intentionally
// remain ABI-stable and describe mechanics; this registry describes authoring
// policy such as prerequisites and whether removal is allowed.
struct EditorComponentPolicy {
    std::string componentType;
    std::vector<std::string> prerequisites;
    bool removable = true;
};

class EditorComponentPolicyRegistry {
public:
    static EditorComponentPolicyRegistry& instance();

    void installDefaults();
    void clear();
    bool registerPolicy(EditorComponentPolicy policy, std::string* error = nullptr);

    const EditorComponentPolicy* find(std::string_view componentType) const;

    // Adds prerequisites first and rolls back additions if any step fails.
    bool addWithRequirements(ayt::entity::Entity& entity,
                             std::string_view componentType,
                             std::vector<std::string>* added,
                             std::string* error) const;

    bool canRemove(const ayt::entity::Entity& entity,
                   std::string_view componentType,
                   std::string* reason = nullptr) const;

private:
    std::unordered_map<std::string, EditorComponentPolicy> _policies;
    bool _defaultsInstalled = false;
};

} // namespace ayt::editor
