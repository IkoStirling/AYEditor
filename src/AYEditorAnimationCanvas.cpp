#include "AYEditorAnimationCanvas.h"

#include <AYResource/assetsImpl/Mesh.h>
#include <AYUI/IRenderBackend.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace ayt::editor {
namespace {

using ayt::anim::editor::AnimationPreviewMode;

constexpr std::size_t kMaximumPreviewTriangles = 1500u;

ayt::math::FVector3 vertexPosition(const ayt::resource::Mesh& mesh,
                                   std::uint32_t index)
{
    ayt::math::FVector3 result{};
    if (index >= mesh.getVertexCount() || mesh.getVertexData() == nullptr) {
        return result;
    }
    const auto info = mesh.getAttributeInfo(ayt::resource::MeshAttribute::Position);
    const auto* source = mesh.getVertexData()
        + static_cast<std::size_t>(index) * mesh.getVertexStride() + info.offset;
    std::memcpy(&result, source, sizeof(result));
    return result;
}

ayt::math::FVector3 skinnedPosition(
    const ayt::resource::Mesh& mesh, std::uint32_t vertex,
    const ayt::resource::SkinPalette* palette,
    const std::vector<ayt::math::Float4x4>& skin)
{
    const ayt::math::FVector3 source = vertexPosition(mesh, vertex);
    const auto* weights = mesh.getSkinWeights();
    if (weights == nullptr || vertex >= mesh.getVertexCount() || skin.empty()) {
        return source;
    }
    const std::uint32_t* paletteJoints = mesh.getSkinPaletteJoints();
    const std::uint32_t paletteJointCount = mesh.getSkinPaletteJointCount();
    ayt::math::FVector3 result{};
    float total = 0.0f;
    for (std::size_t influence = 0; influence < 4u; ++influence) {
        const float weight = weights[vertex].boneWeight[influence];
        if (weight <= 0.0f) continue;
        std::uint32_t joint = weights[vertex].boneIndex[influence];
        if (palette != nullptr && paletteJoints != nullptr
            && joint < palette->jointCount
            && palette->jointOffset + joint < paletteJointCount) {
            joint = paletteJoints[palette->jointOffset + joint];
        }
        if (joint >= skin.size()) continue;
        result += skin[joint].transformPoint(source) * weight;
        total += weight;
    }
    return total > 0.0f ? result * (1.0f / total) : source;
}

} // namespace

EditorAnimationCanvas::EditorAnimationCanvas(
    std::shared_ptr<EditorAnimationDocument> document)
    : _document(std::move(document))
{
    setId("animation_editor_preview_canvas");
}
EditorAnimationCanvas::~EditorAnimationCanvas() { _onControlSelected={}; finishControlDrag(true); }
void EditorAnimationCanvas::finishControlDrag(bool cancel) {
    if (!_controlDragging) return;
    _controlDragging=false; _dragProjection.reset();
    if (_document) {
        if (cancel) (void)_document->cancelAnimationEditGesture();
        else (void)_document->commitAnimationEditGesture();
    }
    _projectionCache.invalidate(); markDirty();
}
void EditorAnimationCanvas::onCaptureCancelled() { finishControlDrag(true); _orbit.cancel(); }

void EditorAnimationCanvas::framePreview()
{
    _orbit.reset();
    _projectionCache.invalidate();
    markDirty();
}

void EditorAnimationCanvas::rebuildModelSegments()
{
    _modelWorldSegments.clear();
    if (_document == nullptr || !_document->preview().canPreviewModel()) return;
    const auto mesh = _document->preview().mesh();
    if (mesh == nullptr || mesh->getIndexData() == nullptr) return;
    const auto& skin = _document->preview().skinMatrices();
    const auto* submeshes = mesh->getSubmeshes();
    const auto* palettes = mesh->getSkinPalettes();
    const std::uint32_t paletteCount = mesh->getSkinPaletteCount();
    std::size_t totalTriangles = 0u;
    for (std::uint32_t section = 0; section < mesh->getSubmeshCount(); ++section) {
        totalTriangles += submeshes[section].indexCount / 3u;
    }
    const std::size_t step = std::max<std::size_t>(1u,
        (totalTriangles + kMaximumPreviewTriangles - 1u)
            / kMaximumPreviewTriangles);
    std::size_t triangleOrdinal = 0u;
    for (std::uint32_t section = 0; section < mesh->getSubmeshCount(); ++section) {
        const auto& submesh = submeshes[section];
        const ayt::resource::SkinPalette* palette =
            palettes != nullptr && section < paletteCount ? &palettes[section] : nullptr;
        const std::uint64_t end = std::min<std::uint64_t>(mesh->getIndexCount(),
            static_cast<std::uint64_t>(submesh.indexOffset) + submesh.indexCount);
        for (std::uint64_t cursor = submesh.indexOffset; cursor + 2u < end;
             cursor += 3u, ++triangleOrdinal) {
            if (triangleOrdinal % step != 0u) continue;
            const auto* indices = mesh->getIndexData();
            const auto a = skinnedPosition(*mesh, indices[cursor], palette, skin);
            const auto b = skinnedPosition(*mesh, indices[cursor + 1u], palette, skin);
            const auto c = skinnedPosition(*mesh, indices[cursor + 2u], palette, skin);
            _modelWorldSegments.push_back({a, b});
            _modelWorldSegments.push_back({b, c});
            _modelWorldSegments.push_back({c, a});
        }
    }
}

void EditorAnimationCanvas::rebuildProjection()
{
    if (_document == nullptr) return;
    const auto bounds = getWorldBounds();
    const auto& preview = _document->preview();
    if (!_projectionCache.consume({preview.revision(), preview.poseRevision(), bounds,
        _orbit.yaw, _orbit.pitch, _orbit.zoom})) return;
    _skeletonWorld.clear();
    _skeletonProjected.clear();
    _modelProjectedSegments.clear();
    _controls.clear();

    const AnimationPreviewMode mode = preview.effectivePreviewMode();
    const bool showSkeleton = mode != AnimationPreviewMode::ModelOnly;
    const bool showModel = mode != AnimationPreviewMode::SkeletonOnly
        && preview.canPreviewModel();
    if (showSkeleton) {
        for (const auto& matrix : preview.poseWorldMatrices()) {
            _skeletonWorld.push_back(matrix.transformPoint({0, 0, 0}));
        }
    }
    if (showModel) rebuildModelSegments();
    else _modelWorldSegments.clear();

    if (const auto* rig=preview.controlRig(); rig && rig->enabled) {
        const auto& world=preview.poseWorldMatrices();
        for (const auto& h:rig->handles()) if (std::size_t(h.bone)<world.size()) {
            bool ikJoint=false; for(unsigned i=0;i<4;++i) if(rig->pose().limbs[i].ik)
                for(unsigned j=0;j<3;++j) ikJoint=ikJoint || rig->limbBone(ayt::anim::editor::RigLimb(i),j)==h.bone;
            if (ikJoint) continue;
            const auto position=world[h.bone].transformPoint({});
            _controls.push_back({"fk."+std::to_string(h.bone)+".rotation",position,{},true});
            if(h.translation) _controls.push_back({"fk."+std::to_string(h.bone)+".position",position,{},false});
        }
        for(unsigned i=0;i<4;++i) if(rig->limbBone(ayt::anim::editor::RigLimb(i),0)>=0 && rig->pose().limbs[i].ik) {
            const auto& limb=rig->pose().limbs[i]; const auto id="ik."+std::to_string(i)+".";
            _controls.push_back({id+"target",limb.target,{},false}); _controls.push_back({id+"pole",limb.pole,{},false});
            _controls.push_back({id+"rotation",limb.target,{},true});
        }
    }

    ayt::ui::authoring::PreviewBounds points;
    for (const auto& point : _skeletonWorld) points.include(point);
    for (const auto& handle:_controls) points.include(handle.world);
    for (const auto& segment : _modelWorldSegments) {
        points.include(segment.a); points.include(segment.b);
    }
    if (!points.populated) return;
    const ayt::ui::authoring::PreviewProjection project=_dragProjection ? *_dragProjection : ayt::ui::authoring::PreviewProjection(points, bounds, _orbit, 0.72f);
    _projection=project;
    for (auto& handle:_controls) handle.projected=project(handle.world);
    _skeletonProjected.reserve(_skeletonWorld.size());
    for (const auto& point : _skeletonWorld) _skeletonProjected.push_back(project(point));
    _modelProjectedSegments.reserve(_modelWorldSegments.size());
    for (const auto& segment : _modelWorldSegments) {
        _modelProjectedSegments.push_back({project(segment.a), project(segment.b)});
    }
}

int EditorAnimationCanvas::hitBone(ayt::math::FVector2 point) const noexcept
{
    int best = -1;
    float bestDistance = 12.0f * 12.0f;
    for (std::size_t index = 0; index < _skeletonProjected.size(); ++index) {
        const float dx = _skeletonProjected[index].x - point.x;
        const float dy = _skeletonProjected[index].y - point.y;
        const float distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(index);
        }
    }
    return best;
}

bool EditorAnimationCanvas::onMouseButtonDown(const ayt::ui::UIMouseEvent& event)
{
    if (event.mouseButton == 1 || event.mouseButton == 2) {
        finishControlDrag(true);
        _orbit.begin(event.mousePos);
        return true;
    }
    if (event.mouseButton != 0) return false;
    rebuildProjection();
    if (_document && _projection && !_document->authoringReadOnly()) {
        for (auto i=_controls.rbegin();i!=_controls.rend();++i) {
            const float dx=event.mousePos.x-i->projected.x,dy=event.mousePos.y-i->projected.y;
            const float radius=std::sqrt(dx*dx+dy*dy);
            if (i->rotation ? std::fabs(radius-14)>5 : radius>8) continue;
            if (!_document->beginAnimationEditGesture("Move rig controller")) return false;
            _document->timelinePause(); _controlId=i->id; _controlDragging=true;
            _dragPointer=event.mousePos; _dragCenter=i->projected; _dragProjection=_projection;
            _dragWorld=_document->preview().poseWorldMatrices();
            auto rig=*_document->preview().controlRig(); (void)rig.captureFK(_dragWorld); _dragPose=rig.pose();
            if (_onControlSelected) _onControlSelected(_controlId);
            markDirty(); return true;
        }
    }
    const int selected = hitBone(event.mousePos);
    if (selected < 0 || _document == nullptr) return false;
    (void)_document->selectBone(selected);
    if (_onBoneSelected) _onBoneSelected(selected);
    markDirty();
    return true;
}

bool EditorAnimationCanvas::onMouseMove(const ayt::ui::UIMouseEvent& event)
{
    if (_controlDragging && _document && _dragProjection) {
        using namespace ayt::math;
        const auto dot=_controlId.find('.',3); const unsigned index=unsigned(std::stoul(_controlId.substr(3,dot-3))); const auto type=_controlId.substr(dot+1);
        const float cy=std::cos(_orbit.yaw),sy=std::sin(_orbit.yaw),cp=std::cos(_orbit.pitch),sp=std::sin(_orbit.pitch);
        const FVector3 right{cy,0,sy},up{sp*sy,cp,-sp*cy},normal{-cp*sy,sp,cp*cy};
        const auto a=(*_dragProjection)(FVector3{}),b=(*_dragProjection)(right);
        const float scale=std::max(.00001f,std::fabs(b.x-a.x));
        const auto delta=right*((event.mousePos.x-_dragPointer.x)/scale)+up*((_dragPointer.y-event.mousePos.y)/scale);
        auto pose=_dragPose;
        if(type=="rotation") {
            const float start=std::atan2(_dragPointer.y-_dragCenter.y,_dragPointer.x-_dragCenter.x);
            const float now=std::atan2(event.mousePos.y-_dragCenter.y,event.mousePos.x-_dragCenter.x);
            const auto rotation=FQuaternion::fromAxisAngle(normal,start-now);
            if(_controlId.starts_with("ik.")) pose.limbs[index].tipRotation=(rotation*pose.limbs[index].tipRotation).normalize();
            else { const int parent=_document->preview().bones()[index].parentIndex; FQuaternion parentQ=FQuaternion::identity(); FVector3 p,s;
                if(parent>=0) _dragWorld[parent].decompose(p,parentQ,s);
                pose.fk.rotations[index]=(parentQ.inverse()*rotation*parentQ*pose.fk.rotations[index]).normalize(); }
        } else if(_controlId.starts_with("ik.")) { if(type=="pole") pose.limbs[index].pole+=delta; else pose.limbs[index].target+=delta; }
        else { const int parent=_document->preview().bones()[index].parentIndex; const auto inv=parent<0 ? Float4x4::identity() : _dragWorld[parent].inverse();
            pose.fk.positions[index]+=inv.transformPoint(delta)-inv.transformPoint({}); }
        const bool edited=_document->editControlRig([&](auto& rig){return rig.setPose(pose);});
        if(edited) markDirty(); return true;
    }
    if (!_orbit.move(event.mousePos)) return false;
    markDirty();
    return true;
}

bool EditorAnimationCanvas::onMouseButtonUp(const ayt::ui::UIMouseEvent& event)
{
    if(event.mouseButton==0 && _controlDragging) {finishControlDrag(false); return true;}
    if (event.mouseButton != 1 && event.mouseButton != 2) return false;
    return _orbit.end();
}

bool EditorAnimationCanvas::onMouseWheel(const ayt::ui::UIMouseWheelEvent& event)
{
    _orbit.wheel(event.deltaY);
    markDirty();
    return true;
}

void EditorAnimationCanvas::onMouseLeave()
{
    finishControlDrag(true);
    _orbit.cancel();
    ayt::ui::Widget::onMouseLeave();
}

ayt::ui::UiCursorHint EditorAnimationCanvas::getCursorHint() const
{
    return _orbit.rotating() || _controlDragging ? ayt::ui::UiCursorHint::Move
                     : ayt::ui::UiCursorHint::Default;
}

void EditorAnimationCanvas::onRender(ayt::ui::IRenderBackend& renderer)
{
    const auto bounds = getWorldBounds();
    renderer.drawRect(bounds, {0.045f, 0.055f, 0.075f, 1.0f});
    rebuildProjection();
    if (_document == nullptr) return;

    if (!_modelProjectedSegments.empty()) {
        const auto path = renderer.createPath();
        if (path.id >= 0) {
            for (const auto& segment : _modelProjectedSegments) {
                const ayt::math::FVector2 points[] = {
                    {segment.a.x, segment.a.y}, {segment.b.x, segment.b.y}};
                renderer.addPathContour(path, points, 2, false);
            }
            renderer.setPathStrokeColor(path, {0.44f, 0.50f, 0.60f, 0.70f});
            renderer.setPathStrokeWidth(path, 1.0f);
            renderer.drawPath(path, ayt::ui::PathFillMode::Stroke);
            renderer.releasePath(path);
        }
    }

    const auto& bones = _document->preview().bones();
    for(const auto& control:_controls) {
        const auto color=control.id==_controlId ? ayt::math::FVector4{1,.8f,.2f,1} : control.id.find("pole")!=std::string::npos ? ayt::math::FVector4{.8f,.4f,1,1} : ayt::math::FVector4{.2f,.85f,.65f,1};
        if(!control.rotation) renderer.drawRect({control.projected.x-4,control.projected.y-4,control.projected.x+4,control.projected.y+4},color);
        else { const auto path=renderer.createPath(); if(path.id>=0) { ayt::math::FVector2 points[32];
            for(int i=0;i<32;++i) {const float angle=i*6.2831853f/32;points[i]={control.projected.x+14*std::cos(angle),control.projected.y+14*std::sin(angle)};}
            renderer.addPathContour(path,points,32,true); renderer.setPathStrokeColor(path,color); renderer.setPathStrokeWidth(path,1.5f); renderer.drawPath(path,ayt::ui::PathFillMode::Stroke);renderer.releasePath(path); }
        }
    }
    if (!_skeletonProjected.empty()) {
        const auto path = renderer.createPath();
        if (path.id >= 0) {
            for (std::size_t index = 0; index < bones.size()
                 && index < _skeletonProjected.size(); ++index) {
                const int parent = bones[index].parentIndex;
                if (parent < 0 || parent >= static_cast<int>(_skeletonProjected.size())) continue;
                const ayt::math::FVector2 points[] = {
                    {_skeletonProjected[static_cast<std::size_t>(parent)].x,
                     _skeletonProjected[static_cast<std::size_t>(parent)].y},
                    {_skeletonProjected[index].x, _skeletonProjected[index].y}};
                renderer.addPathContour(path, points, 2, false);
            }
            renderer.setPathStrokeColor(path, {0.28f, 0.78f, 1.0f, 1.0f});
            renderer.setPathStrokeWidth(path, 2.0f);
            renderer.setPathStrokeStyle(path, ayt::ui::PathStrokeCap::Round,
                ayt::ui::PathStrokeJoin::Round);
            renderer.drawPath(path, ayt::ui::PathFillMode::Stroke);
            renderer.releasePath(path);
        }
        for (std::size_t index = 0; index < _skeletonProjected.size(); ++index) {
            const auto& point = _skeletonProjected[index];
            const bool selected = static_cast<int>(index) == _document->selectedBone();
            const float radius = selected ? 5.0f : 2.5f;
            renderer.drawRect({point.x - radius, point.y - radius,
                               point.x + radius, point.y + radius},
                selected ? ayt::math::FVector4{1.0f, 0.68f, 0.20f, 1.0f}
                         : ayt::math::FVector4{0.80f, 0.88f, 0.96f, 1.0f});
        }
    }

    std::wstring hint = L"LMB select bone  |  RMB/MMB rotate  |  Wheel zoom";
    if (_document->preview().requestedPreviewMode()
            != AnimationPreviewMode::SkeletonOnly
        && !_document->preview().canPreviewModel()) {
        hint += L"  |  Model unavailable - skeleton fallback";
    }
    renderer.drawText({bounds.minX + 8.0f, bounds.minY + 6.0f,
                       bounds.maxX - 8.0f, bounds.minY + 26.0f},
        hint, 11, ayt::math::FVector4{0.58f, 0.64f, 0.72f, 1.0f});
}

} // namespace ayt::editor
