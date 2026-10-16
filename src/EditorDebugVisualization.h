#pragma once
#include <AYEditor/EditorDebugVisualization.h>
#include <AYPhysics/PhysicsDebugDraw.h>
#include <AYEntity/CameraFrame.h>
#include <map>
#include <deque>
#include <set>
namespace ayt::entity { class World; }
namespace ayt::ui { class UIManager; }
namespace ayt::editor {
class EditorDebugVisualization {
public:
    using Record = ayt::physics::PhysicsDebugRecord;
    EditorDebugVisualizationOptions options;
    std::vector<Record> records;
    size_t droppedRecords = 0, visibleLines = 0;
    uint64_t frameIndex = 0;
    bool stepRequested = false;
    std::string inspectedKey;
    ayt::physics::PhysicsDebugDraw contentHistory;
    static std::string recordKey(const Record& record);
    static std::string describe(const Record& record);
    static const char* categoryName(ayt::physics::PhysicsDebugCategory category);
    void update(std::vector<Record> incoming, uint64_t frame, float seconds);
    void clear(int category = -1);
    bool visible(const Record& record, uint32_t selection) const;
    /// Screen-space distance, in pixels; returns a record index or -1.
    int pick(const ayt::entity::CameraFrame& camera, const ayt::entity::CameraViewport& viewport,
             ayt::math::FVector2 pixel, uint32_t selection) const;
private:
    std::set<std::string> _capturedCombatEvents;
    std::map<uint64_t, std::deque<ayt::math::FVector3>> _trails;
};
std::vector<ayt::physics::PhysicsDebugRecord> editorSceneDebugRecords(
    const ayt::entity::World& world, float aspect, uint32_t categories, bool nativeColliders);
std::vector<ayt::physics::PhysicsDebugRecord> editorRuntimeDebugRecords(
    ayt::entity::World& world,uint32_t categories);
std::vector<ayt::physics::PhysicsDebugRecord> editorSimulationDebugRecords(ayt::entity::World& world,uint32_t categories);
std::vector<ayt::physics::PhysicsDebugRecord> editorServiceDebugRecords(ayt::entity::World& world,ayt::ui::UIManager& ui,uint32_t categories);
void clearServiceDebugHistory(ayt::entity::World* world,int category=-1);
void resolveDebugEntities(std::vector<ayt::physics::PhysicsDebugRecord>& records,
                          const ayt::entity::World& world);
}
