#include "EditorDebugVisualization.h"
#include <AYEntity.h>
#include <AYEntity/components/AnimationComponent.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYAnimation/AnimationPlayer.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYAudio/AudioSubSystem.h>
#include <AYAudio/components/AudioEmitterComponent.h>
#include <AYAudio/components/AudioListenerComponent.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYUI/UnicodeText.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <optional>
#include <set>
#if defined(AYEDITOR_HAS_ABILITY)
#include <AYAbility/EntityIntegration.h>
#endif
#if defined(AYEDITOR_HAS_COMBAT)
#include <AYCombat/EntityIntegration.h>
#endif
#if defined(AYEDITOR_HAS_PARTICLE)
#include <AYEntity/ParticleSystem.h>
#include <AYEntity/components/ParticleEmitterComponent.h>
#include <AYEntity/components/ParticleEffectComponent.h>
#endif
#if defined(AYEDITOR_HAS_DETERMINISTIC_DEBUG)
#include <AYApplication.h>
#include <AYEntity/DeterministicHost.h>
#include <AYEntity/DeterministicBrain2D.h>
#endif
namespace ayt::editor {
using namespace ayt::physics;
using V=ayt::math::FVector3;
using Record=PhysicsDebugRecord;
namespace {
constexpr uint32_t green=0xff00ff00,red=0xff0000ff,cyan=0xffffff00,yellow=0xff00ffff;
void marker(Record& r,V p,float size=.15f,uint32_t color=cyan) {
    r.anchor=p;
    r.lines.push_back({p-V{size,0,0},p+V{size,0,0},color});
    r.lines.push_back({p-V{0,size,0},p+V{0,size,0},color});
    r.lines.push_back({p-V{0,0,size},p+V{0,0,size},color});
}
void box(Record& r,V lo,V hi,const ayt::math::Float4x4& matrix,uint32_t color) {
    V points[8];for(int i=0;i<8;++i) points[i]=matrix.transformPoint({i&1?hi.x:lo.x,i&2?hi.y:lo.y,i&4?hi.z:lo.z});
    for(int i=0;i<8;++i) for(int bit=1;bit<8;bit*=2) if(!(i&bit))r.lines.push_back({points[i],points[i|bit],color});
}
void sphere(Record& r,V center,float radius,uint32_t color) {
    if(!std::isfinite(radius) || radius<=0) return;
    ColliderDesc shape;shape.shape=ColliderShape::Sphere;shape.radius=radius;
    auto lines=PhysicsDebugDraw::colliderLines(shape,center,ayt::math::FQuaternion::identity(),PhysicsDimension::ThreeD,color);
    r.lines.insert(r.lines.end(),lines.begin(),lines.end());
}
void cone(Record& r,V origin,V direction,float length,float cosine,uint32_t color) {
    if(!std::isfinite(length)||length<=0 || direction.dot(direction)<1e-8f) return;
    const auto axis=direction.normalize(); const V helper=std::fabs(axis.y)<.9f?V{0,1,0}:V{1,0,0};
    const auto right=axis.cross(helper).normalize(),up=right.cross(axis).normalize();
    // Display the actual spherical range cutoff: outer ring lies on radius=length.
    cosine=std::clamp(cosine,-1.f,1.f);const float radius=length*std::sqrt(std::max(0.f,1-cosine*cosine));
    const auto center=origin+axis*(length*cosine);
    for(int i=0;i<32;++i){const float a=float(i)*6.2831853f/32,b=float(i+1)*6.2831853f/32;
        const auto p=center+(right*std::cos(a)+up*std::sin(a))*radius;
        const auto q=center+(right*std::cos(b)+up*std::sin(b))*radius;
        r.lines.push_back({p,q,color});if(i%8==0)r.lines.push_back({origin,p,color});}
    r.lines.push_back({origin,origin+axis*length,color});
}
Record entityRecord(PhysicsDebugCategory category,ayt::entity::Entity& e,const char* label) {
    Record r;r.category=category;r.entityId=e.getId();r.label=std::string(label)+" | "+(e.getName()?e.getName():"");
    if(auto* transform=e.getComponent<ayt::entity::Transform>()) marker(r,transform->position);
    return r;
}
}
std::vector<Record> editorRuntimeDebugRecords(ayt::entity::World& world,uint32_t categories) {
    PhysicsDebugSnapshot out;out.categories=categories;
    std::optional<ayt::audio::AudioEngine::DebugSnapshot> audioSnapshot;
    std::set<uint64_t> activeVoices;
    if(categories&(1u<<12))if(auto* subsystem=ayt::audio::AudioSubSystem::findRegistered())if(auto* engine=subsystem->engine()) {
        audioSnapshot=engine->debugSnapshot();for(const auto& voice:audioSnapshot->voices)activeVoices.insert(voice.voiceId);
    }
    for(auto* e:world.getAllEntities()) {
        if(!e)continue;auto* transform=e->getComponent<ayt::entity::Transform>();if(!transform)continue;
        const auto matrix=ayt::math::translate(transform->position.x,transform->position.y,transform->position.z)*transform->rotation.toMatrix()*ayt::math::scale(transform->scale.x,transform->scale.y,transform->scale.z);
        if(categories&(1u<<9)) if(auto* skeleton=e->getComponent<ayt::entity::SkeletonComponent>();skeleton && skeleton->player) {
            const auto& player=*skeleton->player;
            auto r=entityRecord(PhysicsDebugCategory::Animation,*e,"Animation playback");
            const auto duration=player.getDuration();std::ostringstream text;
            if(auto* authoring=e->getComponent<ayt::entity::AnimationComponent>())text<<"Authored clip "<<authoring->clipPath<<"\n";
            text<<"Valid "<<player.isValid()<<" | paused "<<player.isPaused()<<" | time "<<player.getTime()<<" / "<<duration
                <<" s | progress "<<(duration>0?std::clamp(player.getTime()/duration,0.f,1.f)*100:0)<<"% | rate "<<player.getPlayRate()
                <<"\nAdditive sources "<<player.getAdditiveLayerCount()<<" | slot weights ";
            for(uint32_t slot=0;slot<ayt::anim::kMaxAdditiveSlots;++slot)text<<slot<<":"<<player.getAdditiveLayerWeight(slot)<<" ";
            for(uint32_t slot=0;slot<ayt::anim::kMaxAdditiveSlots;++slot) {
                const auto& weights=player.getAdditiveLayerTrackWeights(slot);if(weights.empty())continue;
                text<<"\nSlot "<<slot<<" track weights ("<<weights.size()<<"): ";
                for(size_t track=0;track<std::min(weights.size(),size_t(64));++track)text<<track<<":"<<weights[track]<<" ";
                if(weights.size()>64)text<<"...";
            }
            text<<"\nBone mask "<<player.hasSkeletonMask()<<" | generation "<<player.getSkeletonMaskGeneration()<<" | IK chains "<<player.getIKChainCount();
            r.details=text.str();
            if(!player.getBoneCount())r.queryId=999;
            if(player.getBoneCount() && player.getBoneWorldMatrices())r.anchor=matrix.transformPoint(player.getBoneWorldMatrices()[0].transformPoint({}));
            out.append(std::move(r));
            if(skeleton->skeleton && player.getBoneWorldMatrices()) {
                const auto count=std::min(player.getBoneCount(),skeleton->skeleton->getBoneCount());const auto& weights=player.getResolvedBoneMaskWeights();
                for(size_t i=0;i<std::min(count,weights.size());++i) {
                    Record mask;mask.category=PhysicsDebugCategory::Animation;mask.entityId=e->getId();mask.queryId=uint32_t(i+1);
                    mask.label="Bone mask | "+skeleton->skeleton->getBones()[i].name;mask.details="Resolved bone weight "+std::to_string(weights[i]);
                    const auto weight=std::clamp(weights[i],0.f,1.f);const auto color=0xff000000u|uint32_t((1-weight)*255)|(uint32_t(weight*255)<<8);
                    marker(mask,matrix.transformPoint(player.getBoneWorldMatrices()[i].transformPoint({})),.05f,color);out.append(std::move(mask));
                }
            }
            for(uint32_t slot=0;slot<ayt::anim::kMaxIKChains;++slot) {
                const auto& chain=player.getIKChain(slot);if(chain.rootBone.empty())continue;
                auto ik=entityRecord(PhysicsDebugCategory::Animation,*e,"IK target / chain");ik.queryId=1000+slot;
                ik.details=chain.rootBone+" -> "+chain.midBone+" -> "+chain.tipBone+" | weight "+std::to_string(chain.weight)+" | solver "+std::to_string(int(chain.type))+" | iterations "+std::to_string(chain.iterations)+" | active "+std::to_string(player.isIKChainActive(slot));
                const auto target=matrix.transformPoint(chain.targetWorld);
                marker(ik,target,.12f,player.isIKChainActive(slot)?green:yellow);
                if(skeleton->skeleton && player.getBoneWorldMatrices()) for(size_t i=0;i<std::min(player.getBoneCount(),skeleton->skeleton->getBoneCount());++i)
                    if(skeleton->skeleton->getBones()[i].name==chain.tipBone)ik.lines.push_back({matrix.transformPoint(player.getBoneWorldMatrices()[i].transformPoint({})),target,cyan});
                if(skeleton->skeleton && player.getBoneWorldMatrices()) {
                    std::vector<V> joints;
                    for(const auto& name:{chain.rootBone,chain.midBone,chain.tipBone})if(!name.empty())for(size_t i=0;i<std::min(player.getBoneCount(),skeleton->skeleton->getBoneCount());++i)
                        if(skeleton->skeleton->getBones()[i].name==name)joints.push_back(matrix.transformPoint(player.getBoneWorldMatrices()[i].transformPoint({})));
                    for(size_t i=1;i<joints.size();++i)ik.lines.push_back({joints[i-1],joints[i],yellow});
                }
                out.append(std::move(ik));
            }
        }
        if(categories&(1u<<12)) {
            if(auto* emitter=e->getComponent<ayt::audio::AudioEmitterComponent>();emitter && !activeVoices.contains(emitter->voice)) {
                auto r=entityRecord(PhysicsDebugCategory::Audio,*e,"Audio emitter (not playing)");
                r.details="Authored spatial range | no active voice | volume "+std::to_string(emitter->volume)+" | bus "+std::to_string(int(emitter->bus));
                if(emitter->spatial.mode==ayt::audio::SpatializationParams::Mode::World){sphere(r,transform->position,emitter->spatial.referenceDistance,green);sphere(r,transform->position,emitter->spatial.maxDistance,cyan);}
                out.append(std::move(r));
            }
            if(auto* listener=e->getComponent<ayt::audio::AudioListenerComponent>()) {
                auto r=entityRecord(PhysicsDebugCategory::Audio,*e,"Audio listener component");r.details="Primary "+std::to_string(listener->isPrimary)+" | HRTF "+std::to_string(listener->hrtfEnabled)+" | spatial scale "+std::to_string(listener->masterSpatialScale);
                r.lines.push_back({transform->position,transform->position+transform->rotation*V{0,0,-1},green});r.lines.push_back({transform->position,transform->position+transform->rotation*V{0,1,0},cyan});out.append(std::move(r));
            }
        }
        if(categories&(1u<<11)) if(auto* mesh=e->getComponent<ayt::entity::MeshComponent>();mesh && !mesh->visible) {
            auto r=entityRecord(PhysicsDebugCategory::Lighting,*e,"Mesh hidden by component");r.details="Visibility disabled before render submission | LOD index unavailable on this mesh entry";out.append(std::move(r));
        }
#if defined(AYEDITOR_HAS_PARTICLE)
        if(categories&(1u<<13)) {
            const auto parent=ayt::particle::Pose{{transform->position.x,transform->position.y,transform->position.z},
                {matrix(0,0),matrix(1,0),matrix(2,0)},{matrix(0,1),matrix(1,1),matrix(2,1)},{matrix(0,2),matrix(1,2),matrix(2,2)}};
            auto appendEmitter=[&](const ayt::particle::ParticleInstance& instance,const ayt::particle::Pose& pose,bool gpu,const std::string& backend,uint32_t capacity,size_t child) {
                auto r=entityRecord(PhysicsDebugCategory::Particle,*e,"Particle emitter");r.queryId=uint32_t(child);
                const auto& effect=instance.effect();std::ostringstream text;text<<backend<<" | capacity "<<effect.capacity<<" | GPU reserved "<<capacity;
                if(gpu)text<<"\nGPU live count / velocities / age unavailable (no readback)";
                else text<<"\nLive "<<instance.particles().size()<<" | dropped births "<<instance.droppedBirths()<<" | emitting "<<instance.emitting()<<" | paused "<<instance.paused();
                const V center{pose.position.x,pose.position.y,pose.position.z};
                const auto basis=ayt::math::Float4x4::identity();
                auto point=[&](ayt::particle::Vec3 p){const auto v=pose.point(p);return V{v.x,v.y,v.z};};
                auto extent=effect.extent;if(effect.shape==ayt::particle::Shape::Rectangle)extent.z=0;
                if(effect.shape==ayt::particle::Shape::Sphere || effect.shape==ayt::particle::Shape::Disk) {
                    // Nonuniform emitter bases are respected by sampling the authored ellipsoid.
                    for(int plane=0;plane<(effect.shape==ayt::particle::Shape::Disk?1:3);++plane)for(int i=0;i<32;++i){
                        const float a=i*6.2831853f/32,b=(i+1)*6.2831853f/32;
                        auto p=[&](float t){return plane==0?ayt::particle::Vec3{extent.x*std::cos(t),extent.y*std::sin(t),0}:plane==1?ayt::particle::Vec3{extent.x*std::cos(t),0,extent.z*std::sin(t)}:ayt::particle::Vec3{0,extent.y*std::cos(t),extent.z*std::sin(t)};};
                        r.lines.push_back({point(p(a)),point(p(b)),cyan});}
                } else if(effect.shape!=ayt::particle::Shape::Point) {
                    V points[8];for(int i=0;i<8;++i)points[i]=point({i&1?extent.x:-extent.x,i&2?extent.y:-extent.y,i&4?extent.z:-extent.z});
                    for(int i=0;i<8;++i)for(int bit=1;bit<8;bit*=2)if(!(i&bit))r.lines.push_back({points[i],points[i|bit],cyan});
                } else marker(r,center);
                if(!gpu) {
                    V lo{},hi{};bool bounds=false;float minAge=1e30f,maxAge=0,minLife=1e30f,maxLife=0;size_t shown=0;
                    for(const auto& particle:instance.particles()) {
                        const auto sample=instance.sample(particle,pose);const V p{sample.position.x,sample.position.y,sample.position.z};
                        const float radius=std::fabs(sample.size)*.5f;
                        const V half=sample.oriented?V{std::fabs(sample.right.x)+std::fabs(sample.up.x),std::fabs(sample.right.y)+std::fabs(sample.up.y),std::fabs(sample.right.z)+std::fabs(sample.up.z)}*radius:V{1.414214f,1.414214f,1.414214f}*radius;
                        for(int axis=0;axis<3;++axis){lo[axis]=bounds?std::min(lo[axis],p[axis]-half[axis]):p[axis]-half[axis];hi[axis]=bounds?std::max(hi[axis],p[axis]+half[axis]):p[axis]+half[axis];}bounds=true;
                        minAge=std::min(minAge,particle.age);maxAge=std::max(maxAge,particle.age);minLife=std::min(minLife,particle.life);maxLife=std::max(maxLife,particle.life);
                        if(shown++<64){const auto v=effect.space==ayt::particle::Space::Local?pose.vector(particle.velocity):particle.velocity;r.lines.push_back({p,p+V{v.x,v.y,v.z}*.1f,green});}
                    }
                    if(bounds) {box(r,lo,hi,basis,yellow);text<<"\nAge "<<minAge<<".."<<maxAge<<" s | lifetime "<<minLife<<".."<<maxLife<<" s";}
                    text<<"\nConservative visual bounds; simulation velocities exclude sway/visual height.\nVelocity samples "<<std::min(shown,size_t(64))<<" (0.1 s vectors)";
                }
                r.details=text.str();out.append(std::move(r));
            };
            if(auto* emitter=e->getComponent<ayt::entity::ParticleEmitterComponent>()) {
                if(emitter->runtime)appendEmitter(*emitter->runtime,parent,emitter->usingGpu,emitter->backendStatus,emitter->gpuCapacity,0);
                if(!emitter->error.empty()){auto r=entityRecord(PhysicsDebugCategory::Particle,*e,"Particle error");r.details=emitter->error;out.append(std::move(r));}
            }
            if(auto* effect=e->getComponent<ayt::entity::ParticleEffectComponent>()) {
                if(effect->runtime)for(size_t i=0;i<effect->runtime->instances().size();++i)appendEmitter(effect->runtime->instances()[i],effect->runtime->emitterPose(i,parent),effect->usingGpu,effect->backendStatus,effect->gpuCapacity,i+1);
                if(!effect->error.empty()){auto r=entityRecord(PhysicsDebugCategory::Particle,*e,"Particle effect error");r.details=effect->error;out.append(std::move(r));}
            }
        }
#endif
    }
#if defined(AYEDITOR_HAS_COMBAT)
    if(auto* combat=dynamic_cast<ayt::combat::CombatSystem*>(world.findSystemByName("CombatSystem"))) {
        combat->setDebugCaptureEnabled((categories&(1u<<10))!=0);
        if(categories&(1u<<10)) {
            auto handle=[](ayt::combat::CombatantRef ref){return ayt::entity::EntityHandle{uint32_t(ref.object),uint32_t(ref.generation-1)};};
            for(const auto& event:combat->debugOutcomes()) {
                const auto& hit=event.outcome;auto* e=world.getEntityByHandle(handle(hit.target));if(!e)continue;
                auto r=entityRecord(PhysicsDebugCategory::Combat,*e,"Combat result");r.queryId=uint32_t(event.sequence);r.completedAtUs=event.completedAtUs;
                static const char* kinds[]={"Rejected","Blocked","Zero","Damage","Heal"};
                std::ostringstream text;text<<kinds[size_t(hit.kind)]<<" | tick "<<event.tick<<" | calculated "<<hit.calculated<<" | remaining "<<hit.remaining
                    <<"\nPermission "<<hit.decision.allowed<<" | denial "<<int(hit.decision.denial)<<" | rule action "<<int(hit.rule.action)<<" | rule reason "<<hit.rule.reason
                    <<"\nAttack "<<hit.attack.id<<" | request "<<hit.request<<" | ability "<<hit.source.ability;
                for(const auto& change:hit.settlement.changes)text<<"\nActual delta "<<change.actual;
                static const char* denials[]={"None","Inactive","Missing capability","Self forbidden","Unknown relation","Relation forbidden","Environment forbidden"};
                text<<"\n"<<denials[size_t(hit.decision.denial)];
                if(hit.rule.reason==1)text<<" | Invulnerable";else if(hit.rule.reason==2)text<<" | Damage type immune";else if(hit.rule.reason==3)text<<" | Blocking";
                r.details=text.str();if(hit.source.instigator)r.otherEntityId=uint32_t(hit.source.instigator->object);
                for(auto& line:r.lines)line.color=hit.state==ayt::combat::OutcomeState::Applied?red:green;
                out.append(std::move(r));
            }
            for(const auto& event:combat->debugInteractions()) {
                const auto& interaction=event.interaction;auto* e=world.getEntityByHandle(handle(interaction.target));if(!e)continue;
                auto r=entityRecord(PhysicsDebugCategory::Combat,*e,"Attribute / effect interaction");r.queryId=uint32_t(event.sequence);r.completedAtUs=event.completedAtUs;
                static const char* denials[]={"None","Inactive","Missing capability","Not selected","Missing required tag","Effect immunity","Self forbidden","Unknown relation","Relation forbidden","Environment forbidden"};
                r.details="Tick "+std::to_string(event.tick)+" | allowed "+std::to_string(interaction.decision.allowed)+" | "+denials[size_t(interaction.decision.denial)]+" | effect instance "+std::to_string(interaction.effectInstance);
                if(interaction.attribute)r.details+="\nBefore "+std::to_string(interaction.attribute->before)+" | after "+std::to_string(interaction.attribute->after)+" | actual delta "+std::to_string(interaction.attribute->actual);
                if(interaction.source.instigator)r.otherEntityId=uint32_t(interaction.source.instigator->object);
                for(auto& line:r.lines)line.color=interaction.decision.allowed?red:green;out.append(std::move(r));
            }
            for(const auto& effect:combat->debugEffects())if(auto* e=world.getEntityByHandle(effect.target)) {
                auto r=entityRecord(PhysicsDebugCategory::Combat,*e,"Owned effect");r.otherEntityId=effect.source.id;r.queryId=uint32_t(effect.instance);
                r.details="Instance "+std::to_string(effect.instance)+" | remaining ticks "+(effect.remainingTicks==UINT64_MAX?std::string("permanent"):std::to_string(effect.remainingTicks));out.append(std::move(r));
            }
        }
    }
#endif
#if defined(AYEDITOR_HAS_ABILITY)
    if(categories&(1u<<10))if(auto* ability=dynamic_cast<ayt::ability::AbilitySystem*>(world.findSystemByName("AbilitySystem")))for(const auto& state:ability->debugRecords())if(auto* e=world.getEntityByHandle(state.actor)) {
        auto r=entityRecord(PhysicsDebugCategory::Combat,*e,state.label.c_str());r.details=state.details;out.append(std::move(r));
    }
#endif
    if(audioSnapshot) {
        const auto& snapshot=*audioSnapshot;
        Record listener;listener.category=PhysicsDebugCategory::Audio;listener.label="Audio listener / mixer";marker(listener,snapshot.listener.position);
        listener.lines.push_back({snapshot.listener.position,snapshot.listener.position+snapshot.listener.forward,green});listener.lines.push_back({snapshot.listener.position,snapshot.listener.position+snapshot.listener.up,cyan});
        std::ostringstream text;text<<"Active voices "<<snapshot.voices.size()<<" | paused "<<snapshot.paused<<" | master gain "<<snapshot.masterGain
            <<"\nOutput L/R peak "<<snapshot.output.left<<" / "<<snapshot.output.right<<" | clipping "<<snapshot.output.clipped;
        const char* busNames[]={"Master","Music","Sfx","UI","Voice"};
        for(size_t i=0;i<snapshot.busGains.size();++i)text<<"\nBus "<<busNames[i]<<" gain "<<snapshot.busGains[i]<<" | dry peak "<<snapshot.busPeaks[i];
        listener.details=text.str();out.append(std::move(listener));
        for(const auto& voice:snapshot.voices) {
            Record r;r.category=PhysicsDebugCategory::Audio;r.label="Audio voice "+std::to_string(voice.voiceId);r.queryId=uint32_t(voice.voiceId);
            V p=voice.spatial.position;if(voice.spatial.mode==ayt::audio::SpatializationParams::Mode::Relative)p=p+snapshot.listener.position;
            marker(r,p);if(voice.spatial.mode!=ayt::audio::SpatializationParams::Mode::Disabled){sphere(r,p,voice.spatial.referenceDistance,green);sphere(r,p,voice.spatial.maxDistance,cyan);}
            r.lines.push_back({p,p+voice.spatial.forward,green});
            r.details="Bus "+std::to_string(int(voice.bus))+" | volume "+std::to_string(voice.volume)+" | pitch "+std::to_string(voice.pitch)+" | paused "+std::to_string(voice.paused)+" | looping "+std::to_string(voice.loop)+"\nSource frame "+std::to_string(voice.positionFrames)+" | envelope gain "+std::to_string(voice.envelopeGain)+" | spatial blend "+std::to_string(voice.spatial.spatialBlend)+" | attenuation model "+std::to_string(int(voice.spatial.attenuation));
            for(auto* e:world.getAllEntities())if(e)if(auto* emitter=e->getComponent<ayt::audio::AudioEmitterComponent>();emitter && emitter->voice==voice.voiceId){r.entityId=e->getId();break;}
            out.append(std::move(r));
        }
    }
    if(categories&(1u<<11))if(auto* subsystem=ayt::render::RendererSubSystem::findRegistered()) {
        const auto snapshot=subsystem->renderer().debugSnapshot();
        static const char* states[]={"Visible","Outside camera frustum","Culling bypassed (deformation / unknown bounds / opt-out)","Invalid mesh/material handle","Material missing"};
        for(const auto& draw:snapshot.draws)if(draw.world==reinterpret_cast<uintptr_t>(&world)) {
            Record r;r.category=PhysicsDebugCategory::Lighting;r.entityId=draw.entityId;r.label="Render mesh "+std::to_string(draw.mesh);
            r.details=std::string(states[size_t(draw.visibility)])+" | render frame "+std::to_string(snapshot.frame)+"\nLOD index unavailable on this mesh entry | "+(draw.deformed?"Bind-pose bounds (deformed surface can differ)":"Static mesh bounds");
            if(draw.boundsValid)for(int i=0;i<8;++i)for(int bit=1;bit<8;bit*=2)if(!(i&bit))r.lines.push_back({draw.corners[i],draw.corners[i|bit],draw.visibility==ayt::render::DebugDrawVisibility::FrustumCulled?red:green});
            out.append(std::move(r));
        }
        for(uint32_t i=0;i<snapshot.lights.count;++i) {
            const auto& light=snapshot.lights.lights[i];Record r;r.category=PhysicsDebugCategory::Lighting;r.queryId=i;r.label="Light "+std::to_string(i);marker(r,light.position);
            r.details="Type "+std::to_string(int(light.type))+" | intensity "+std::to_string(light.intensity)+" | range "+std::to_string(light.range)+" | shadow requested "+std::to_string(light.castShadow)+" | shadows enabled "+std::to_string(snapshot.shadowsEnabled);
            if(light.type==ayt::render::LightType::Point){sphere(r,light.position,light.range,yellow);if(light.castShadow)r.details+="\nPoint omni shadows unsupported";}
            else if(light.type==ayt::render::LightType::Spot){cone(r,light.position,light.spotDirection,light.range,light.coneCosOuter,yellow);cone(r,light.position,light.spotDirection,light.range,light.coneCosInner,cyan);}
            else r.lines.push_back({light.position,light.position+light.direction.normalize()*2,cyan});
            out.append(std::move(r));
        }
        for(size_t slot=0;slot<snapshot.shadowViewProjections.size();++slot) {
            const auto inverse=snapshot.shadowViewProjections[slot].inverse_fast();V points[8];bool valid=true;
            for(int i=0;i<8;++i){const auto p=inverse*ayt::math::FVector4{i&1?1.f:-1.f,i&2?1.f:-1.f,i&4?1.f:snapshot.homogeneousDepth?-1.f:0.f,1};if(std::fabs(p.w)<1e-8f){valid=false;break;}points[i]={p.x/p.w,p.y/p.w,p.z/p.w};}
            if(!valid)continue;Record r;r.category=PhysicsDebugCategory::Lighting;r.label="Shadow projection "+std::to_string(slot);r.details="Actual produced shadow atlas projection | render frame "+std::to_string(snapshot.frame);
            for(int i=0;i<8;++i)for(int bit=1;bit<8;bit*=2)if(!(i&bit))r.lines.push_back({points[i],points[i|bit],yellow});out.append(std::move(r));
        }
    }
#if defined(AYEDITOR_HAS_PARTICLE)
    if(categories&(1u<<13))if(auto* system=dynamic_cast<ayt::entity::ParticleSimulationSystem*>(world.findSystemByName("ParticleSimulationSystem"))) {
        Record r;r.category=PhysicsDebugCategory::Particle;r.label="World particle budget";r.details="CPU live "+std::to_string(system->liveParticles)+" | GPU reserved "+std::to_string(system->gpuReservedParticles)+" | budget "+std::to_string(system->maxParticles)+" | GPU emitters "+std::to_string(system->gpuEmitters);out.append(std::move(r));
    }
#endif
#if defined(AYEDITOR_HAS_DETERMINISTIC_DEBUG)
    if(categories&(1u<<14))if(auto* host=ayt::app::currentEngineHost())if(auto* controller=ayt::entity::deterministicHost(*host))if(const auto* session=controller->session()) {
        if(const auto checkpoint=controller->checkpoint())for(const auto& config:session->debugBrainConfigs2D())for(const auto& owner:config->owners) {
            auto* e=session->presentationEntity(owner.actor);if(e && world.findEntity(e->getId())!=e)continue;
            const auto state=ayt::entity::detBrainState2D(*checkpoint,*config,owner.actor);
            Record r;r.category=PhysicsDebugCategory::Brain;r.dimension=PhysicsDimension::TwoD;r.label="2D decision owner "+std::to_string(owner.actor);if(e)r.entityId=e->getId();
            static const char* modes[]={"None","Patrol","Chase","Attack requested","Busy","Dead","Retired","Sight blocked"};
            r.details=std::string(modes[size_t(state.mode)])+" | decision count "+std::to_string(state.decisionCount)+" | last decision tick "+std::to_string(state.lastDecisionTick)+"\nFixed target "+std::to_string(owner.target)+" | target present "+std::to_string(state.observedTargetPresent)+" | alive "+std::to_string(state.observedTargetAlive)+"\nDesired speed "+std::to_string(state.desiredSpeed.toFloat())+" | attack held "+std::to_string(state.desiredAttackHeld)+" | LOS policy "+std::to_string(config->lineOfSight)+"\nHistorical pre-motion observation; movement segment is intent, not navigation.";
            if(!state.decisionCount){r.details+="\nNo decision has run yet";out.append(std::move(r));continue;}
            const V p{state.observedOwnerPosition.x.toFloat(),state.observedOwnerPosition.y.toFloat(),0};const V half{owner.senseHalfExtent.x.toFloat(),owner.senseHalfExtent.y.toFloat(),0};marker(r,p);
            box(r,p-half,p+half,ayt::math::Float4x4::identity(),state.mode==ayt::entity::DetBrainMode2D::SightBlocked?red:green);
            r.lines.push_back({{owner.patrolLeft.toFloat(),p.y,0},{owner.patrolRight.toFloat(),p.y,0},yellow});
            r.lines.push_back({p,p+V{state.desiredSpeed.toFloat(),0,0},cyan});
            if(state.observedTargetPresent){const V target{state.observedTargetPosition.x.toFloat(),state.observedTargetPosition.y.toFloat(),0};r.lines.push_back({p,target,state.mode==ayt::entity::DetBrainMode2D::SightBlocked?red:green});marker(r,target,.1f,yellow);if(auto* targetEntity=session->presentationEntity(owner.target))r.otherEntityId=targetEntity->getId();}
            out.append(std::move(r));
        }
    }
#endif
    return std::move(out.records);
}
}
