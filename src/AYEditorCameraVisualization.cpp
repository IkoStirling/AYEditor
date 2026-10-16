#include <AYEditor/EditorCameraVisualization.h>
#include <AYEntity/World.h>
#include <AYEntity.h>
#include <AYEntity/PerspectiveCameraSelection.h>
#include <AYEntity/components/PerspectiveCameraComponent.h>
#include <AYRenderer.h>
#include <AYRenderer/RenderScene.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/assetsImpl/Material.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace ayt::editor {
using namespace ayt::math;
namespace {
bool finite(const FVector3& p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool valid(const FQuaternion& q) { return std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w)&&std::isfinite(q.dot(q))&&q.dot(q)>1e-12f; }
}
std::vector<EditorCameraVisual> buildEditorCameraVisuals(ayt::entity::World& world,
    const EditorSceneCameraFrame& view, float viewportHeight, float aspect,
    uint32_t selectedEntity, EditorCameraDisplayMode mode, bool edit3D) {
    std::vector<EditorCameraVisual> result;
    if (!edit3D || mode==EditorCameraDisplayMode::Hidden || !std::isfinite(viewportHeight)
        || viewportHeight<=0 || !finite(view.position)) return result;
    const float sy=std::fabs(view.projection(1,1));
    if (!std::isfinite(sy)||sy<1e-6f) return result;
    aspect=std::isfinite(aspect)&&aspect>1e-6f?aspect:1.f;
    std::map<int,ayt::entity::PerspectiveCameraComponent*> active;
    for (auto* entity:world.getAllEntities()) {
        FVector3 position; FQuaternion orientation;
        if (!entity || !entity->worldPose(position,orientation) || !finite(position)||!valid(orientation)) continue;
        for (auto* lens:entity->getComponents<ayt::entity::PerspectiveCameraComponent>()) {
            if (!lens || !finite(lens->localOffset)||!valid(lens->localRotation)) continue;
            const auto eye=position+orientation.normalize()*lens->localOffset;
            const auto rotation=(orientation.normalize()*lens->localRotation.normalize()).normalize();
            if (!finite(eye)) continue;
            const auto eyeInView=view.view.transformPoint(eye);
            if (!finite(eyeInView)||eyeInView.z<=1e-4f) continue;
            const float unitsPerPixel=2.f*(view.orthographic?1.f:eyeInView.z)/(sy*viewportHeight);
            EditorCameraVisual visual;
            visual.entity=entity; visual.camera=lens;
            // Canonical model's vertical height is .574 m, independent of entity scale.
            visual.scale=std::clamp(unitsPerPixel*48.f/.574f,.001f,10000.f);
            visual.pose=Float4x4::fromTRS(eye,rotation,{1,1,1});
            visual.iconWorld=Float4x4::fromTRS(eye,rotation,{visual.scale,visual.scale,visual.scale});
            visual.selected=entity->getId()==selectedEntity;
            if (!active.contains(lens->viewportId))
                active[lens->viewportId]=ayt::entity::selectPerspectiveCamera3D(world,lens->viewportId).camera;
            visual.runtimeActive=active[lens->viewportId]==lens;
            const float nearPlane=std::isfinite(lens->nearZ)&&lens->nearZ>=1e-4f&&lens->nearZ<=1e6f?lens->nearZ:.1f;
            const float farPlane=std::isfinite(lens->farZ)&&lens->farZ>nearPlane&&lens->farZ<=1e8f?lens->farZ:nearPlane+100.f;
            const auto projection=lens->projectionMatrix(aspect);
            for (int plane=0;plane<2;++plane) {
                const float z=plane?farPlane:nearPlane;
                const float factor=lens->projectionMode==1?1.f:z;
                const float w=factor/projection(0,0), h=factor/projection(1,1);
                visual.frustum[plane*4+0]={-w,-h,z}; visual.frustum[plane*4+1]={w,-h,z};
                visual.frustum[plane*4+2]={w,h,z}; visual.frustum[plane*4+3]={-w,h,z};
            }
            result.push_back(visual);
        }
    }
    return result;
}
bool hitEditorCameraVisual(const EditorCameraVisual& visual,const FVector3& origin,const FVector3& direction,float& distance) {
    if (!finite(origin)||!finite(direction)||direction.lengthSq()<1e-12f) return false;
    auto inverse=visual.iconWorld.inverse();
    auto p=inverse.transformPoint(origin),d=inverse.transformDirection(direction.normalize());
    float low=0,high=1e30f;
    const float a[3]={p.x,p.y,p.z}, b[3]={d.x,d.y,d.z};
    const float minimum[3]={-.329f,-.235f,-.565f},maximum[3]={.329f,.350f,.004f};
    for (int i=0;i<3;++i) {
        if (std::fabs(b[i])<1e-9f) { if (a[i]<minimum[i]||a[i]>maximum[i]) return false; continue; }
        float t0=(minimum[i]-a[i])/b[i],t1=(maximum[i]-a[i])/b[i];
        if(t0>t1)std::swap(t0,t1); low=std::max(low,t0);high=std::min(high,t1);
        if(low>high)return false;
    }
    distance=low;return std::isfinite(distance);
}

struct EditorCameraVisualRenderer::Impl {
    struct Section { uint32_t first=0,count=0; ayt::render::MaterialHandle materials[3]; };
    struct Asset { ayt::render::MeshHandle mesh; std::vector<Section> sections; bool tried=false; };
    struct Frustum { std::array<FVector3,8> corners{}; ayt::render::MeshHandle mesh; };
    Asset assets[2]; std::vector<Frustum> frustums;
    ayt::render::MaterialHandle frustumMaterial;
    std::string root;
};
EditorCameraVisualRenderer::EditorCameraVisualRenderer():_impl(std::make_unique<Impl>()){}
EditorCameraVisualRenderer::~EditorCameraVisualRenderer()=default;
bool EditorCameraVisualRenderer::ready(EditorCameraDisplayMode mode) const noexcept {
    if(mode==EditorCameraDisplayMode::Hidden)return false;
    const auto& asset=_impl->assets[mode==EditorCameraDisplayMode::Wireframe?1:0];
    return asset.mesh.isValid() && std::any_of(asset.sections.begin(),asset.sections.end(),[](const Impl::Section& s){return s.materials[0].isValid()&&s.materials[1].isValid()&&s.materials[2].isValid();});
}

void EditorCameraVisualRenderer::release(ayt::render::Renderer& renderer) {
    for(auto& asset:_impl->assets) {
        if(asset.mesh.isValid())renderer.destroyMesh(asset.mesh);
        for(auto& section:asset.sections)for(auto& material:section.materials)if(material.isValid())renderer.destroyMaterial(material);
    }
    for(auto& frustum:_impl->frustums)if(frustum.mesh.isValid())renderer.destroyMesh(frustum.mesh);
    if(_impl->frustumMaterial.isValid())renderer.destroyMaterial(_impl->frustumMaterial);
    _impl=std::make_unique<Impl>();
}
namespace {
// Unlit strokes; modest directional modulation keeps the solid camera's shape readable.
constexpr const char* iconShader=R"(
material EditorCamera {
 property baseColor = vec4(1.0,1.0,1.0,1.0)
 property shading = vec4(0.0,0.0,0.0,0.0)
 vertex {
  in pos : position
  in nrm : normal
  out worldNormal : normal = (modelMatrix * vec4(nrm,0.0)).xyz
  return modelViewProjection * vec4(pos,1.0)
 }
 fragment {
  in worldNormal : normal
  let light = 0.65 + 0.35 * abs(dot(normalize(worldNormal),normalize(vec3(0.3,0.7,-0.4))))
  return vec4(baseColor.rgb * (1.0 - shading.x + shading.x * light),baseColor.a)
 }
}
)";
struct Vertex {float x,y,z,nx,ny,nz;};
ayt::render::MeshHandle strokeMesh(ayt::render::Renderer& renderer,const std::array<FVector3,8>& corners,float radius) {
    std::vector<Vertex> vertices;std::vector<uint32_t> indices;
    constexpr int edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for(auto& edge:edges) {
        auto a=corners[edge[0]],b=corners[edge[1]],delta=b-a;
        if(!finite(a)||!finite(b)||delta.lengthSq()<1e-10f)continue;
        auto z=delta.normalize();auto x=z.cross(std::fabs(z.y)<.9f?FVector3{0,1,0}:FVector3{1,0,0}).normalize()*radius;
        auto y=z.cross(x).normalize()*radius;
        const auto start=static_cast<uint32_t>(vertices.size());
        for(auto p:{a+x,a+y,a-x,a-y,b+x,b+y,b-x,b-y})vertices.push_back({p.x,p.y,p.z,0,1,0});
        for(uint32_t i=0;i<4;++i) {uint32_t j=(i+1)%4; for(auto index:{i,j,j+4,i,j+4,i+4})indices.push_back(start+index);}
    }
    if(vertices.empty())return {};
    return renderer.createMesh32(vertices.data(),static_cast<uint32_t>(vertices.size()),
        ayt::render::VertexLayoutDesc::position3Normal3(),indices.data(),static_cast<uint32_t>(indices.size()),false);
}
}
void EditorCameraVisualRenderer::append(ayt::render::Renderer& renderer,ayt::render::RenderScene& scene,
    const std::string& engineAssetsRoot,const std::vector<EditorCameraVisual>& visuals,EditorCameraDisplayMode mode) {
    if(_impl->root!=engineAssetsRoot) {release(renderer);_impl->root=engineAssetsRoot;}
    const int variant=mode==EditorCameraDisplayMode::Wireframe?1:0;
    auto& asset=_impl->assets[variant];
    if(!visuals.empty()&&!asset.tried) {
        asset.tried=true;
        auto root=std::filesystem::path(engineAssetsRoot)/"AYEditor/models/CameraOrientation/Cooked";
        const std::string filename=variant?"camera_orientation_wire_RootNode_CameraOrientationWire.msh":"camera_orientation_RootNode_CameraOrientation.msh";
        auto source=ayt::resource::ResourceManager::instance().load<ayt::resource::IMesh>((root/"meshes"/filename).string());
        if(source&&std::filesystem::is_regular_file(root/"meshes"/filename)
            && !ayt::resource::ResourceManager::instance().hasLoadFailed((root/"meshes"/filename).string())
            &&source->hasAttribute(ayt::resource::MeshAttribute::Position)&&source->hasAttribute(ayt::resource::MeshAttribute::Normal)
            &&source->getVertexCount()&&source->getIndexCount()) {
            std::vector<Vertex> vertices(source->getVertexCount());
            auto position=source->getAttributeInfo(ayt::resource::MeshAttribute::Position);
            auto normal=source->getAttributeInfo(ayt::resource::MeshAttribute::Normal);
            for(uint32_t i=0;i<source->getVertexCount();++i) {
                auto p=source->getVertexData()+i*source->getVertexStride();
                std::memcpy(&vertices[i].x,p+position.offset,12);std::memcpy(&vertices[i].nx,p+normal.offset,12);
            }
            // Own upload avoids global triangle wireframe changing the authored contour icon.
            asset.mesh=renderer.createMesh32(vertices.data(),source->getVertexCount(),ayt::render::VertexLayoutDesc::position3Normal3(),source->getIndexData(),source->getIndexCount(),false);
            for(uint32_t i=0;i<source->getSubmeshCount();++i) {
                auto sub=source->getSubmeshes()[i];Impl::Section section;section.first=sub.indexOffset;section.count=sub.indexCount;
                if(sub.materialIndex>=source->getMaterialSlotCount())continue;
                auto mat=ayt::resource::ResourceManager::instance().load<ayt::resource::IMaterial>((root/source->getMaterialSlot(sub.materialIndex)).string());
                if(!mat||!std::filesystem::is_regular_file(root/source->getMaterialSlot(sub.materialIndex))||ayt::resource::ResourceManager::instance().hasLoadFailed((root/source->getMaterialSlot(sub.materialIndex)).string()))continue;
                auto color=mat->getColor("baseColor");
                for(int state=0;state<3;++state) {
                    const std::string key="editor-camera-icon/"+engineAssetsRoot+"/"+std::to_string(variant)+"/"+std::to_string(i)+"/"+std::to_string(state)+"/"+std::to_string(reinterpret_cast<uintptr_t>(_impl.get()));
                    auto material=renderer.createMaterialFromPhoskia(iconShader,key);
                    const float factor=state==2?.35f:state==0?1.3f:1.f;
                    const float alpha=variant?(state==0?1.f:state==2?.35f:.65f):1.f;
                    renderer.setMaterialColor(material,"baseColor",color.x*factor,color.y*factor,color.z*factor,alpha);
                    renderer.setMaterialColor(material,"shading",variant?0.f:1.f,0,0,0);
                    renderer.setMaterialBlendMode(material,variant?ayt::render::BlendMode::Alpha:ayt::render::BlendMode::Opaque);
                    renderer.setMaterialModel(material,ayt::render::MaterialModel::Unlit);
                    section.materials[state]=material;
                }
                asset.sections.push_back(section);
            }
        }
    }
    size_t frustumIndex=0;
    for(const auto& visual:visuals) {
        const int state=visual.selected?0:visual.runtimeActive?1:2;
        if(asset.mesh.isValid())for(const auto& section:asset.sections) {
            if(!section.materials[state].isValid())continue;
            ayt::render::DrawItem item;item.mesh=asset.mesh;item.material=section.materials[state];item.world=visual.iconWorld;
            item.firstIndex=section.first;item.indexCount=section.count;item.shadowFlags=ayt::render::ShadowFlags::None;item.cameraLayerMask=0xFFFFFFFFu;
            scene.add(item);
        }
        if(!visual.selected)continue;
        if(!_impl->frustumMaterial.isValid()) {
            _impl->frustumMaterial=renderer.createMaterialFromPhoskia(iconShader,"editor-camera-frustum/"+std::to_string(reinterpret_cast<uintptr_t>(_impl.get())));
            renderer.setMaterialColor(_impl->frustumMaterial,"baseColor",1.f,.35f,.035f,1.f);
            renderer.setMaterialSurfaceProperties(_impl->frustumMaterial,0,.5f,true);
            renderer.setMaterialModel(_impl->frustumMaterial,ayt::render::MaterialModel::Unlit);
        }
        if(_impl->frustums.size()<=frustumIndex)_impl->frustums.resize(frustumIndex+1);
        auto& frustum=_impl->frustums[frustumIndex++];
        if(!frustum.mesh.isValid()||!std::equal(frustum.corners.begin(),frustum.corners.end(),visual.frustum.begin(),[](const FVector3& a,const FVector3& b){return a.x==b.x&&a.y==b.y&&a.z==b.z;})) {
            if(frustum.mesh.isValid())renderer.destroyMesh(frustum.mesh);
            frustum.corners=visual.frustum;frustum.mesh=strokeMesh(renderer,visual.frustum,.006f);
        }
        if(frustum.mesh.isValid()&&_impl->frustumMaterial.isValid()) {
            ayt::render::DrawItem item;item.mesh=frustum.mesh;item.material=_impl->frustumMaterial;item.world=visual.pose;
            item.shadowFlags=ayt::render::ShadowFlags::None;item.cameraLayerMask=0xFFFFFFFFu;scene.add(item);
        }
    }
    while(_impl->frustums.size()>frustumIndex) {
        auto& last=_impl->frustums.back();if(last.mesh.isValid())renderer.destroyMesh(last.mesh);_impl->frustums.pop_back();
    }
}
}
