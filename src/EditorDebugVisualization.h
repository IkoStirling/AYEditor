#pragma once
#include <AYEditor/EditorDebugVisualization.h>
#include <AYPhysics/PhysicsDebugDraw.h>
#include <AYEntity/CameraFrame.h>
#include <map>
#include <deque>
namespace ayt::entity { class World; }
namespace ayt::editor {
class EditorDebugVisualization {
public:
    using Record = ayt::physics::PhysicsDebugRecord;
    EditorDebugVisualizationOptions options;
    std::vector<Record> records;
    size_t droppedRecords = 0, visibleLines = 0;
    uint64_t frameIndex = 0;
    bool stepRequested = false;
    void update(std::vector<Record> incoming, uint64_t frame, float seconds);
    void clear(int category = -1);
    bool visible(const Record& record, uint32_t selection) const;
    /// Screen-space distance, in pixels; returns a record index or -1.
    int pick(const ayt::entity::CameraFrame& camera, const ayt::entity::CameraViewport& viewport,
             ayt::math::FVector2 pixel, uint32_t selection) const;
private:
    std::map<uint64_t, std::deque<ayt::math::FVector3>> _trails;
};
std::vector<ayt::physics::PhysicsDebugRecord> editorSceneDebugRecords(
    const ayt::entity::World& world, float aspect, uint32_t categories, bool nativeColliders);
void resolveDebugEntities(std::vector<ayt::physics::PhysicsDebugRecord>& records,
                          const ayt::entity::World& world);
}
