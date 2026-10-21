#pragma once

#include <AYEditor/EditorSceneCamera.h>
#include <AYEntity.h>
#include <AYEntity/CameraFrame.h>
#include <AYEntity/components/LightComponent.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ayt::editor
{
// Transient owner-thread presentation data. Entity pointers never outlive this call's World.
struct EditorLightVisual
{
    entity::Entity* object = nullptr;
    math::FVector2 center{};
    float depth = 0;
    int32_t type = 0;
    bool enabled = false;
    bool selected = false;
};
struct EditorLightLine
{
    math::FVector2 from, to;
};
struct EditorLightVisuals
{
    math::FRectangle bounds{};
    std::vector<EditorLightVisual> icons;
    std::vector<EditorLightLine> selectedLines;
};
inline EditorLightVisuals buildEditorLightVisuals(entity::World& world, const EditorSceneCameraFrame& frame,
                                                  const math::FRectangle& bounds, uint32_t selection)
{
    EditorLightVisuals result;
    result.bounds = bounds;
    if (bounds.width() <= 0 || bounds.height() <= 0)
        return result;
    entity::CameraFrame camera;
    camera.view = frame.view;
    camera.projection = frame.projection;
    const entity::CameraViewport viewport{bounds.minX, bounds.minY, bounds.width(), bounds.height()};
    // Clip wire segments in homogeneous space before dividing, including the near plane.
    const auto line = [&](math::FVector3 a, math::FVector3 b) {
        const auto ca = camera.projection * (camera.view * math::FVector4{a.x, a.y, a.z, 1});
        const auto cb = camera.projection * (camera.view * math::FVector4{b.x, b.y, b.z, 1});
        const float fa[] = {ca.x + ca.w, ca.w - ca.x, ca.y + ca.w, ca.w - ca.y,
                            ca.z,        ca.w - ca.z, ca.w - 1e-6f};
        const float fb[] = {cb.x + cb.w, cb.w - cb.x, cb.y + cb.w, cb.w - cb.y,
                            cb.z,        cb.w - cb.z, cb.w - 1e-6f};
        float low = 0, high = 1;
        for (int i = 0; i < 7; ++i)
        {
            if (!std::isfinite(fa[i]) || !std::isfinite(fb[i]) || (fa[i] < 0 && fb[i] < 0))
                return;
            if ((fa[i] < 0) != (fb[i] < 0))
            {
                const float t = fa[i] / (fa[i] - fb[i]);
                if (fa[i] < 0)
                    low = std::max(low, t);
                else
                    high = std::min(high, t);
            }
        }
        if (low > high)
            return;
        const auto delta = b - a;
        math::FVector3 p, q;
        if (entity::worldToScreen(camera, viewport, a + delta * low, p) &&
            entity::worldToScreen(camera, viewport, a + delta * high, q))
            result.selectedLines.push_back({{p.x, p.y}, {q.x, q.y}});
    };
    for (auto* object : world.query<entity::LightComponent>())
    {
        auto* light = object->getComponent<entity::LightComponent>();
        math::FVector3 position;
        math::FQuaternion rotation;
        if (!object->worldPose(position, rotation))
            continue;
        math::FVector3 pixel;
        if (entity::worldToScreen(camera, viewport, position, pixel) && pixel.z >= 0 && pixel.z <= 1 &&
            pixel.x >= bounds.minX + 8 && pixel.x <= bounds.maxX - 8 && pixel.y >= bounds.minY + 8 &&
            pixel.y <= bounds.maxY - 8)
            result.icons.push_back({object,
                                    {pixel.x, pixel.y},
                                    pixel.z,
                                    light->type,
                                    light->enabled,
                                    object->getId() == selection});
        if (object->getId() != selection || !light->valid())
            continue;
        const auto matrix = rotation.toMatrix();
        const auto point = [&](math::FVector3 local) { return position + matrix.transformDirection(local); };
        if (light->type == int32_t(entity::SceneLightType::Directional))
        {
            line(position, point({0, 0, 2}));
            line(point({0, 0, 2}), point({.2f, 0, 1.7f}));
            line(point({0, 0, 2}), point({-.2f, 0, 1.7f}));
        }
        else if (light->type == int32_t(entity::SceneLightType::Point))
        {
            for (int axis = 0; axis < 3; ++axis)
                for (int i = 0; i < 32; ++i)
                {
                    const auto ring = [&](int index) {
                        const float angle = float(index) * 6.28318530718f / 32;
                        const float x = std::cos(angle) * light->range, y = std::sin(angle) * light->range;
                        return point(axis == 0   ? math::FVector3{x, y, 0}
                                     : axis == 1 ? math::FVector3{x, 0, y}
                                                 : math::FVector3{0, x, y});
                    };
                    line(ring(i), ring(i + 1));
                }
        }
        else
        {
            // The renderer attenuates by radial distance: the cone ends on the range sphere.
            for (float degrees : {light->innerAngleDegrees, light->outerAngleDegrees})
            {
                const float angle = degrees * .0174532925199433f, radius = light->range * std::sin(angle),
                            z = light->range * std::cos(angle);
                const auto ring = [&](int i) {
                    const float a = float(i) * 6.28318530718f / 32;
                    return point({radius * std::cos(a), radius * std::sin(a), z});
                };
                for (int i = 0; i < 32; ++i)
                    line(ring(i), ring(i + 1));
                if (degrees == light->outerAngleDegrees)
                    for (int i = 0; i < 32; i += 8)
                        line(position, ring(i));
            }
            line(position, point({0, 0, light->range}));
        }
    }
    // Stable far-to-near overlay order; picking traverses the same list in reverse.
    std::sort(result.icons.begin(), result.icons.end(), [](const auto& a, const auto& b) {
        return a.depth != b.depth ? a.depth > b.depth : a.object->persistentId() > b.object->persistentId();
    });
    return result;
}
inline entity::Entity* pickEditorLightVisual(const EditorLightVisuals& visuals, math::FVector2 pixel)
{
    for (auto it = visuals.icons.rbegin(); it != visuals.icons.rend(); ++it)
        if (std::fabs(pixel.x - it->center.x) <= 8 && std::fabs(pixel.y - it->center.y) <= 8)
            return it->object;
    return nullptr;
}
} // namespace ayt::editor
