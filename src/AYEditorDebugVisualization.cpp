#include "EditorDebugVisualization.h"
#include <AYEntity.h>
#include <AYEntity/components/RigidBodyComponent.h>
#include <AYEntity/components/ColliderComponent.h>
#include <AYEntity/components/PerspectiveCameraComponent.h>
#include <AYEntity/components/OrthoCameraComponent.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYAnimation/AnimationPlayer.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <array>
#if defined(AYEDITOR_HAS_COMBAT)
#include <AYCombat/EntityIntegration.h>
#endif
namespace ayt::editor {
using namespace ayt::physics;
using V=ayt::math::FVector3;
namespace {
uint64_t identity(const PhysicsDebugRecord& r) {return (uint64_t(r.dimension)<<32)|r.body;}
std::vector<PhysicsDebugRecord> balance(std::vector<PhysicsDebugRecord> input) {
    std::array<std::vector<PhysicsDebugRecord>,kPhysicsDebugCategoryCount> groups;
    for(auto& row:input)if(size_t(row.category)<groups.size())groups[size_t(row.category)].push_back(std::move(row));
    std::vector<PhysicsDebugRecord> output;output.reserve(input.size());
    for(size_t index=0;;++index){bool any=false;for(auto& group:groups)if(index<group.size()){output.push_back(std::move(group[index]));any=true;}if(!any)break;}
    return output;
}
bool finite(V v) {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
}
std::string EditorDebugVisualization::recordKey(const Record& r) {
    return std::to_string(int(r.category))+":"+std::to_string(r.entityId)+":"+std::to_string(r.otherEntityId)+":"+std::to_string(int(r.dimension))+":"+std::to_string(r.body)+":"+std::to_string(r.collider)+":"+std::to_string(r.queryId)+":"+r.label;
}
const char* EditorDebugVisualization::categoryName(PhysicsDebugCategory category) {
    static const char* names[]={"Collider","Query","Contact","Character","Motion","Joint","Camera","Skeleton","Ability region","Animation","Combat / stages","Lighting / render","Audio","Particle","2D decision","Deterministic physics","Paths / interactions","Checkpoints / respawn","Stats / calculations","Camera behavior","UI layout / routing","Input","Network","Sequence","Resources"};
    const auto index=size_t(category);return index<std::size(names)?names[index]:"Unknown";
}
std::string EditorDebugVisualization::describe(const Record& r) {
    std::string text=std::string(categoryName(r.category))+" | "+r.label+"\nEntity="+std::to_string(r.entityId);
    if(uint32_t(r.category)<6 || r.category==PhysicsDebugCategory::Ability) {
        text+=" body="+std::to_string(r.body)+" query="+std::to_string(r.queryId)+"\nDistance="+std::to_string(r.distance)+" mask="+std::to_string(r.layerMask)+"\nHit="+std::to_string(r.hit)+" status="+toString(r.status)+"\nPoint=("+std::to_string(r.hitPoint.x)+","+std::to_string(r.hitPoint.y)+","+std::to_string(r.hitPoint.z)+")\nNormal=("+std::to_string(r.normal.x)+","+std::to_string(r.normal.y)+","+std::to_string(r.normal.z)+")\nImpulse="+(r.impulseAvailable?std::to_string(r.impulse):"unavailable");
    }
    if(!r.details.empty())text+="\n"+r.details;return text;
}
bool EditorDebugVisualization::visible(const Record& r,uint32_t selection) const {
    if(uint32_t(r.category)>=kPhysicsDebugCategoryCount)return false;
    const bool query=r.category==PhysicsDebugCategory::Query || r.category==PhysicsDebugCategory::Ability;
    const bool physical=uint32_t(r.category)<6;
    const bool layer=query?(options.layerMask&r.layerMask)!=0:!physical||(r.layer<32 && (options.layerMask&(1u<<r.layer)));
    return (options.categories&(1u<<uint32_t(r.category))) && layer &&
        (!options.selectedOnly || (selection && (r.entityId==selection || r.otherEntityId==selection || std::find(r.relatedEntities.begin(),r.relatedEntities.end(),selection)!=r.relatedEntities.end())));
}
void EditorDebugVisualization::update(std::vector<Record> incoming,uint64_t frame,float seconds) {
    if(options.frozen && !stepRequested) {
        _capturedCombatEvents.clear();
        for(const auto& r:incoming)if((r.transientEvent || (r.category==PhysicsDebugCategory::Combat && r.completedAtUs)))_capturedCombatEvents.insert(recordKey(r));
        return;
    }
    if(options.frozen && frame==frameIndex) return;
    incoming=balance(std::move(incoming));
    stepRequested=false;
    const bool next=frame!=frameIndex; frameIndex=frame;
    std::set<uint64_t> live;
    for(auto& r:incoming) if((r.category==PhysicsDebugCategory::Motion && r.body) ||
        (r.category==PhysicsDebugCategory::Animation && r.entityId && r.queryId==0 && r.label.starts_with("Animation playback"))) {
        const auto key=r.category==PhysicsDebugCategory::Motion?identity(r):(uint64_t(1)<<63)|r.entityId;live.insert(key);
        if(!_trails.contains(key) && _trails.size()>=256) {++droppedRecords;continue;}
        auto& trail=_trails[key];
        if(next && finite(r.anchor)) trail.push_back(r.anchor);
        while(trail.size()>options.trailFrames) trail.pop_front();
        for(size_t i=1;i<trail.size();++i) r.lines.push_back({trail[i-1],trail[i],0xffffff00});
        if(trail.size()>1 && std::isfinite(seconds) && seconds>0) {
            const auto old=trail[trail.size()-2];
            r.lines.push_back({old,r.anchor,0xff00ff00});
            if(r.category==PhysicsDebugCategory::Motion)r.lines.push_back({old,old+r.desiredVelocity*seconds,0xffff0000});
        }
    }
    for(auto i=_trails.begin();i!=_trails.end();) if(!live.contains(i->first)) i=_trails.erase(i);else ++i;
    std::set<std::string> events;
    for(auto& r:incoming) if(uint32_t(r.category)>=9) {
        if((r.transientEvent || (r.category==PhysicsDebugCategory::Combat && r.completedAtUs))) {
            const auto key=recordKey(r);events.insert(key);
            if(_capturedCombatEvents.contains(key))continue;
        }
        (void)contentHistory.recordGeometry(std::move(r));
    }
    _capturedCombatEvents=std::move(events);
    std::erase_if(incoming,[](const auto& r){return uint32_t(r.category)>=9;});
    for(auto& r:contentHistory.consumeRecords()) if(options.categories&(1u<<uint32_t(r.category))) incoming.push_back(std::move(r));
    incoming=balance(std::move(incoming));
    records.clear();visibleLines=0;
    for(auto& r:incoming) {
        if(records.size()>=PhysicsDebugDraw::kMaxRecords || r.lines.size()+r.screenLines.size()>PhysicsDebugDraw::kMaxLines-visibleLines) {++droppedRecords;continue;}
        if(std::any_of(r.lines.begin(),r.lines.end(),[](auto& l){return !finite(l.from)||!finite(l.to);}) || std::any_of(r.screenLines.begin(),r.screenLines.end(),[](auto& l){return !finite(l.from)||!finite(l.to);})) {++droppedRecords;continue;}
        visibleLines+=r.lines.size()+r.screenLines.size();records.push_back(std::move(r));
    }
}
void EditorDebugVisualization::clear(int category) {
    if(category<0) {contentHistory.clear();_capturedCombatEvents.clear();inspectedKey.clear();records.clear();_trails.clear();droppedRecords=0;visibleLines=0;return;}
    const auto prefix=std::to_string(category)+":";std::erase_if(_capturedCombatEvents,[&](const auto& key){return key.starts_with(prefix);});
    contentHistory.clearCategory(static_cast<PhysicsDebugCategory>(category));
    std::erase_if(records,[category](const auto& r){return int(r.category)==category;});
    if(category==int(PhysicsDebugCategory::Motion) || category==int(PhysicsDebugCategory::Animation)) _trails.clear();
    visibleLines=0;for(const auto& r:records) visibleLines+=r.lines.size()+r.screenLines.size();
}
int EditorDebugVisualization::pick(const ayt::entity::CameraFrame& camera,const ayt::entity::CameraViewport& viewport,
    ayt::math::FVector2 pixel,uint32_t selection) const {
    float best=36;int found=-1;
    for(size_t i=0;i<records.size();++i) if(visible(records[i],selection)) for(const auto& l:records[i].lines) {
        V a,b;
        const auto ca=camera.projection*(camera.view*ayt::math::FVector4{l.from.x,l.from.y,l.from.z,1});
        const auto cb=camera.projection*(camera.view*ayt::math::FVector4{l.to.x,l.to.y,l.to.z,1});
        float low=0,high=1;bool clipped=false;
        const float fa[]={ca.x+ca.w,ca.w-ca.x,ca.y+ca.w,ca.w-ca.y,ca.z,ca.w-ca.z,ca.w-1e-6f};
        const float fb[]={cb.x+cb.w,cb.w-cb.x,cb.y+cb.w,cb.w-cb.y,cb.z,cb.w-cb.z,cb.w-1e-6f};
        for(int plane=0;plane<7;++plane) {
            if(!std::isfinite(fa[plane])||!std::isfinite(fb[plane])||(fa[plane]<0 && fb[plane]<0)) {clipped=true;break;}
            if((fa[plane]<0)!=(fb[plane]<0)) {const float t=fa[plane]/(fa[plane]-fb[plane]);if(fa[plane]<0)low=std::max(low,t);else high=std::min(high,t);}
        }
        if(clipped || low>high) continue;
        const auto delta=l.to-l.from;
        if(!ayt::entity::worldToScreen(camera,viewport,l.from+delta*low,a)||!ayt::entity::worldToScreen(camera,viewport,l.from+delta*high,b)) continue;
        const float dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
        const float t=length>1e-8f?std::clamp(((pixel.x-a.x)*dx+(pixel.y-a.y)*dy)/length,0.f,1.f):0;
        const float x=a.x+dx*t-pixel.x,y=a.y+dy*t-pixel.y,distance=x*x+y*y;
        if(distance<best) {best=distance;found=int(i);}
    }
    return found;
}
void resolveDebugEntities(std::vector<PhysicsDebugRecord>& records,const ayt::entity::World& world) {
    std::map<uint64_t,std::pair<uint32_t,std::string>> bodies;
    for(auto* e:world.getAllEntities()) if(e) if(auto* body=e->getComponent<ayt::entity::RigidBodyComponent>()) {
        PhysicsDebugRecord r;r.body=body->getBodyHandle();
        r.dimension=body->getPhysicsDimension()==ayt::entity::RigidBodyComponent::PhysicsDimension::TwoD?PhysicsDimension::TwoD:PhysicsDimension::ThreeD;
        if(r.body) bodies.emplace(identity(r),std::pair{e->getId(),std::string(e->getName()?e->getName():"")});
    }
    std::map<uint64_t,PhysLayer> layers;
    for(const auto& r:records) if(r.category==PhysicsDebugCategory::Motion) layers[identity(r)]=r.layer;
    for(auto& r:records) {
        if(r.body && layers.contains(identity(r))) r.layer=layers.at(identity(r));
        if(r.otherBody) {const auto found=bodies.find((uint64_t(r.dimension)<<32)|r.otherBody);if(found!=bodies.end())r.otherEntityId=found->second.first;}
        for(const auto body:r.hitBodies) {const auto found=bodies.find((uint64_t(r.dimension)<<32)|body);if(found!=bodies.end())r.relatedEntities.push_back(found->second.first);}
        if(r.entityId || !r.body) continue;
        auto found=bodies.find(identity(r));if(found!=bodies.end()) {r.entityId=found->second.first;r.label+=" | "+found->second.second;}
    }
}
std::vector<PhysicsDebugRecord> editorSceneDebugRecords(const ayt::entity::World& world,float aspect,uint32_t categories,bool nativeColliders) {
    PhysicsDebugSnapshot result;
    for(auto* e:world.getAllEntities()) {
        if(!e) continue;auto* pose=e->getComponent<ayt::entity::Transform>();if(!pose) continue;
        const auto matrix=ayt::math::translate(pose->position.x,pose->position.y,pose->position.z)*pose->rotation.toMatrix()*ayt::math::scale(pose->scale.x,pose->scale.y,pose->scale.z);
        if((categories&1) && (!nativeColliders || !e->getComponent<ayt::entity::RigidBodyComponent>() || !e->getComponent<ayt::entity::RigidBodyComponent>()->getBodyHandle())) {
            if(auto* collider=e->getComponent<ayt::entity::ColliderComponent>()) for(const auto& s:collider->shapes) {
                PhysicsDebugRecord r;r.category=PhysicsDebugCategory::Collider;r.entityId=e->getId();r.label="Authored collider";
                if(auto* body=e->getComponent<ayt::entity::RigidBodyComponent>()) {
                    r.dimension=body->getPhysicsDimension()==ayt::entity::RigidBodyComponent::PhysicsDimension::TwoD?PhysicsDimension::TwoD:PhysicsDimension::ThreeD;
                }
                ColliderDesc shape;shape.halfExtents=s.halfExtents;shape.radius=s.radius;shape.height=s.height;shape.offset=s.offset;
                switch(s.shape) {case ayt::entity::ColliderShapeType::Box:shape.shape=ColliderShape::Box;break;
                    case ayt::entity::ColliderShapeType::Sphere:shape.shape=ColliderShape::Sphere;break;
                    case ayt::entity::ColliderShapeType::Capsule:shape.shape=ColliderShape::Capsule;break;default:continue;}
                r.lines=PhysicsDebugDraw::colliderLines(shape,pose->position,pose->rotation,r.dimension,s.isTrigger?0xffffff00:0xff00c8ff);result.append(std::move(r));
            }
        }
        if(categories&(1u<<6)) {
            auto appendCamera=[&](const ayt::entity::CameraFrame& frame) {
                PhysicsDebugRecord r;r.category=PhysicsDebugCategory::Camera;r.entityId=e->getId();r.label="Camera frustum / near-far clip";
                V points[8];bool valid=true;
                for(int i=0;i<8;++i) valid=ayt::entity::screenToWorld(frame,{0,0,1,1},{float(i&1),float((i>>1)&1),float((i>>2)&1)},points[i])&&valid;
                if(valid) for(int i=0;i<8;++i) for(int bit=1;bit<8;bit*=2) if(!(i&bit)) r.lines.push_back({points[i],points[i|bit],0xffff8000});
                result.append(std::move(r));
            };
            for(auto* c:e->getComponents<ayt::entity::PerspectiveCameraComponent>()) if(c) {
                const auto rotation=pose->rotation.normalize();
                ayt::entity::CameraFrame frame;
                frame.view=c->viewMatrix(pose->position+rotation*c->localOffset,(rotation*c->localRotation.normalize()).normalize());
                frame.projection=c->projectionMatrix(aspect);appendCamera(frame);
            }
            for(auto* c:e->getComponents<ayt::entity::OrthoCameraComponent>()) if(c) {
                ayt::entity::CameraFrame frame;frame.view=c->viewMatrix(*pose);frame.projection=c->projectionMatrix(aspect);appendCamera(frame);
            }
        }
        if(categories&(1u<<7)) if(auto* s=e->getComponent<ayt::entity::SkeletonComponent>();s && s->skeleton) {
            const size_t count=s->skeleton->getBoneCount();
            const auto* animated=s->player && s->player->getBoneCount()>=count?s->player->getBoneWorldMatrices():nullptr;
            const auto* inverse=s->skeleton->getInverseBindMatrices();
            if(!animated && !inverse) continue;
            for(size_t i=0;i<count;++i) {
                const auto bone=matrix*(animated?animated[i]:inverse[i].inverse_fast());
                PhysicsDebugRecord r;r.category=PhysicsDebugCategory::Skeleton;r.entityId=e->getId();r.queryId=uint32_t(i);
                r.label="Bone "+s->skeleton->getBones()[i].name;r.anchor=bone.transformPoint({});
                const auto parent=s->skeleton->getParentBoneIndex(i);
                if(parent>=0 && size_t(parent)<count) {const auto m=matrix*(animated?animated[parent]:inverse[parent].inverse_fast());r.lines.push_back({m.transformPoint({}),r.anchor,0xffffff00});}
                for(int axis=0;axis<3;++axis) {V v{};if(axis==0)v.x=.1f;else if(axis==1)v.y=.1f;else v.z=.1f;
                    r.lines.push_back({r.anchor,bone.transformPoint(v),axis==0?0xff0000ffu:axis==1?0xff00ff00u:0xffff0000u});}
                result.append(std::move(r));
            }
        }
    }
#if defined(AYEDITOR_HAS_COMBAT)
    if(categories&(1u<<8)) if(auto* system=dynamic_cast<ayt::combat::CombatSystem*>(world.findSystemByName("CombatSystem"))) {
        for(const auto& area:system->debugAreas()) for(const auto& target:area.sweep.targets) {
            auto* entity=world.findEntity(target.target.id);auto* pose=entity?entity->getComponent<ayt::entity::Transform>():nullptr;
            if(!pose)continue;
            PhysicsDebugRecord r;r.category=PhysicsDebugCategory::Ability;r.entityId=target.target.id;r.anchor=pose->position;
            const bool affected=target.state==ayt::combat::AreaTargetState::Entered || target.state==ayt::combat::AreaTargetState::Retained;
            r.label="Area "+std::to_string(area.sweep.area)+(affected?" affected target":" rejected/exited/failed target");
            const auto color=affected?0xff0000ffu:0xff00ff00u;
            r.lines={{r.anchor-V{.2f,0,0},r.anchor+V{.2f,0,0},color},{r.anchor-V{0,.2f,0},r.anchor+V{0,.2f,0},color}};
            result.append(std::move(r));
        }
    }
#endif
    return std::move(result.records);
}
}
