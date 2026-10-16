#include "EditorDebugVisualization.h"
#include "EditorDebugPrimitives.h"
#include <AYEntity.h>
#if defined(AYEDITOR_HAS_DETERMINISTIC_DEBUG)
#include <AYApplication.h>
#include <AYEntity/DeterministicHost.h>
#include <AYEntity/DeterministicDebug2D.h>
#include <AYEntity/DeterministicCollision2D.h>
#include <AYEntity/DeterministicCharacterController2D.h>
#include <AYEntity/DeterministicPathMotion2D.h>
#include <AYEntity/DeterministicInteraction2D.h>
#include <AYEntity/DeterministicCharacterRespawnFlow2D.h>
#endif
#include <sstream>
#include <algorithm>
namespace ayt::editor {
using namespace debug_detail;
std::vector<Record> editorSimulationDebugRecords(ayt::entity::World& world,uint32_t categories) {
    Collector out;out.categories=categories;
#if defined(AYEDITOR_HAS_DETERMINISTIC_DEBUG)
    using namespace ayt::entity;using D=ayt::math::DetFloat32;using DV=ayt::math::DetVec2;
    if(!(categories&((1u<<15)|(1u<<16)|(1u<<17)|(1u<<22))))return {};
    auto* host=ayt::app::currentEngineHost();auto* controller=host?deterministicHost(*host):nullptr;
    const auto* session=controller?controller->session():nullptr;if(!session)return {};
    const auto* policies=session->debugPolicies2D();const auto cp=controller->checkpoint();if(!cp)return {};
    // A Session from another inspected World must not leak records into this viewport.
    bool matching=cp->actors.empty();for(const auto& [id,actor]:cp->actors)if(auto* e=session->presentationEntity(id)){matching=world.findEntity(e->getId())==e;break;}if(!matching)return {};
    auto vec=[](DV v){return V{v.x.toFloat(),v.y.toFloat(),0};};
    auto position=[&](SimEntityId id){const auto found=cp->actors.find(id);return found==cp->actors.end()?V{}:V{D::fromBits(found->second.pose.position[0]).toFloat(),D::fromBits(found->second.pose.position[1]).toFloat(),0};};
    auto entityId=[&](SimEntityId id){auto* e=session->presentationEntity(id);return e && world.findEntity(e->getId())==e?e->getId():0;};
    auto row=[&](Category category,SimEntityId id,const std::string& label){Record r;r.category=category;r.dimension=ayt::physics::PhysicsDimension::TwoD;r.entityId=entityId(id);r.label=label+" | actor "+std::to_string(id);r.anchor=position(id);return r;};
    try {
    if(policies && enabled(categories,Category::DeterministicPhysics)) {
        for(const auto& config:policies->collisions){const auto schema=detCollisionBodySchema2D(*config);
            for(const auto& [id,actor]:cp->actors){const auto found=actor.blocks.find(config->bodySchema);if(found==actor.blocks.end())continue;const auto& words=found->second;
                const auto mode=readDetState<uint32_t>(schema,words,DetBodyMode);if(!mode)continue;
                const auto half=vec(readDetState<DV>(schema,words,DetBodyHalf)),offset=vec(readDetState<DV>(schema,words,DetBodyOffset));const auto center=position(id)+offset;
                const auto trigger=readDetState<bool>(schema,words,DetBodyTrigger);auto r=row(Category::DeterministicPhysics,id,trigger?"Deterministic trigger":"Deterministic solid");
                const auto shape=config->staticSlopes?readDetState<uint32_t>(schema,words,DetBodyShape):0;const auto color=trigger?yellow:cyan;
                if(!shape)box(r,center-half,center+half,color);
                else {const V a{center.x-half.x,center.y-half.y,0},b{center.x+half.x,center.y-half.y,0};const V top{shape==1?center.x+half.x:center.x-half.x,center.y+half.y,0};r.lines={{a,b,color},{b,top,color},{top,a,color}};}
                const auto velocity=vec(readDetState<DV>(schema,words,DetBodyVelocity));r.lines.push_back({center,center+velocity*.1f,green});
                std::ostringstream text;text<<"Committed next tick "<<cp->nextTick<<" | mode "<<mode<<" | shape "<<shape<<" | layer "<<readDetState<uint32_t>(schema,words,DetBodyLayer)<<" | mask "<<readDetState<uint32_t>(schema,words,DetBodyMask);
                if(config->oneWayPlatforms)text<<"\nOne-way "<<readDetState<bool>(schema,words,DetBodyOneWay)<<" | ignored platform "<<readDetState<DetEntityRef>(schema,words,DetBodyIgnoredOneWay).value;
                if(config->motionPolicy!=DetCollisionMotionPolicy2D::Fault)text<<"\nMotion result "<<readDetState<uint32_t>(schema,words,DetBodyMotionResult)<<" | other "<<readDetState<DetEntityRef>(schema,words,DetBodyMotionOther).value;
                r.details=text.str();out.append(std::move(r));
            }
        }
        for(const auto& config:policies->characters){const auto schema=detCharacterStateSchema2D(*config);
            for(const auto& [id,actor]:cp->actors){const auto found=actor.blocks.find(config->stateSchema);if(found==actor.blocks.end())continue;const auto& words=found->second;
                if(!readDetState<bool>(schema,words,DetCharacterEnabled))continue;
                auto r=row(Category::DeterministicPhysics,id,"Deterministic character state");const bool grounded=readDetState<bool>(schema,words,DetCharacterGrounded);const auto support=readDetState<DetEntityRef>(schema,words,DetCharacterSupport).value;
                const auto own=vec(readDetState<DV>(schema,words,DetCharacterVelocity)),carry=vec(readDetState<DV>(schema,words,DetCharacterSupportVelocity));
                marker(r,r.anchor,grounded?green:red);r.lines.push_back({r.anchor,r.anchor+own*.1f,green});r.lines.push_back({r.anchor,r.anchor+carry*.1f,cyan});
                if(support && cp->actors.contains(support)){r.otherEntityId=entityId(support);r.lines.push_back({r.anchor,position(support),yellow});}
                std::ostringstream text;text<<"Grounded "<<grounded<<" | support "<<support<<" | own velocity ("<<own.x<<","<<own.y<<") | carrier ("<<carry.x<<","<<carry.y<<")";
                if(config->advanced){text<<"\nCoyote opportunities "<<readDetState<uint32_t>(schema,words,DetCharacterCoyoteRemaining)<<" | jump buffer "<<readDetState<uint32_t>(schema,words,DetCharacterJumpBufferRemaining)<<" | drop ticks "<<readDetState<uint32_t>(schema,words,DetCharacterDropRemaining)<<" | crouched "<<readDetState<bool>(schema,words,DetCharacterCrouched);
                    if(readDetState<bool>(schema,words,DetCharacterStandingCaptured)){const auto half=vec(readDetState<DV>(schema,words,DetCharacterStandingHalf)),offset=vec(readDetState<DV>(schema,words,DetCharacterStandingOffset));box(r,r.anchor+offset-half,r.anchor+offset+half,yellow);text<<"\nStanding envelope guide; no new clearance query";}}
                if(config->collision.staticSlopes){const auto normal=vec(readDetState<DV>(schema,words,DetCharacterGroundNormal));r.lines.push_back({r.anchor,r.anchor+normal,yellow});}
                text<<"\nGround probe "<<config->groundProbeDistance.toFloat()<<" | snap "<<config->snapDistance.toFloat();r.details=text.str();out.append(std::move(r));
            }
        }
    }
    if(policies && enabled(categories,Category::PathInteraction)) {
        for(const auto& config:policies->paths){const auto schema=detPathMotionStateSchema2D(*config);
            for(const auto& [id,actor]:cp->actors){const auto found=actor.blocks.find(config->stateSchema);if(found==actor.blocks.end() || !readDetState<bool>(schema,found->second,DetPathBound))continue;const auto& words=found->second;
                auto r=row(Category::PathInteraction,id,"Path platform / moving trigger");const auto origin=vec(readDetState<DV>(schema,words,DetPathOrigin));const auto waypoint=readDetState<uint32_t>(schema,words,DetPathWaypoint);const auto blocked=readDetState<bool>(schema,words,DetPathBlocked);
                for(size_t i=0;i<config->waypoints.size();++i){const auto p=origin+vec(config->waypoints[i].offset);marker(r,p,i==waypoint?red:cyan,.08f);if(i)r.lines.push_back({origin+vec(config->waypoints[i-1].offset),p,blocked?red:green});}
                if(config->playback==DetPathPlayback2D::Loop)r.lines.push_back({origin+vec(config->waypoints.back().offset),origin,green});
                r.details="Waypoint "+std::to_string(waypoint)+" | direction "+std::to_string(readDetState<int32_t>(schema,words,DetPathDirection))+" | elapsed ticks "+std::to_string(readDetState<uint32_t>(schema,words,DetPathElapsedTicks))+" | dwell ticks "+std::to_string(readDetState<uint32_t>(schema,words,DetPathDwellRemaining))+"\nBlocked "+std::to_string(blocked)+" | paused "+std::to_string(readDetState<bool>(schema,words,DetPathPaused))+" | finished "+std::to_string(readDetState<bool>(schema,words,DetPathFinished));out.append(std::move(r));
            }
        }
        for(const auto& config:policies->interactions)for(const auto& rule:config->rules){const auto state=detInteractionState2D(*cp,*config,rule.id);auto r=row(Category::PathInteraction,rule.actor,"Trigger interaction rule "+std::to_string(rule.id));r.otherEntityId=entityId(rule.trigger);
            if(cp->actors.contains(rule.actor)&&cp->actors.contains(rule.trigger))r.lines.push_back({position(rule.trigger),position(rule.actor),state.pending?red:green});
            const auto cooldown=state.cooldownUntil>cp->nextTick?state.cooldownUntil-cp->nextTick:0;
            r.details="Phase "+std::to_string(int(rule.phase))+" | once consumed "+std::to_string(state.fired)+" | pending "+std::to_string(state.pending)+" | cooldown ticks "+std::to_string(cooldown)+"\nScheduled "+std::to_string(state.scheduleCount)+" | attempts "+std::to_string(state.attemptCount)+" | due tick "+std::to_string(state.dueTick)+" | last result "+std::to_string(int(state.lastResult));out.append(std::move(r));
        }
        for(const auto& config:policies->collisions)for(const auto& event:cp->pendingEvents)if(event.source==config->systemId && event.type==config->eventType){DetTriggerEvent2D trigger;if(!decodeDetTriggerEvent2D(event.payload,trigger))continue;
            auto r=row(Category::PathInteraction,trigger.pair.first,"Pending trigger phase "+std::to_string(int(trigger.phase))+" @next tick "+std::to_string(cp->nextTick));r.queryId=event.sequence;r.transientEvent=true;r.otherEntityId=entityId(trigger.pair.second);
            if(cp->actors.contains(trigger.pair.first)&&cp->actors.contains(trigger.pair.second))r.lines.push_back({position(trigger.pair.first),position(trigger.pair.second),trigger.phase==DetTriggerPhase2D::Exit?green:red});
            r.details="Real collision event; processing at next tick "+std::to_string(cp->nextTick)+" | pair "+std::to_string(trigger.pair.first)+" / "+std::to_string(trigger.pair.second);out.append(std::move(r));
        }
    }
    if(policies && enabled(categories,Category::Respawn)) {
        for(const auto& config:policies->respawns)for(const auto& spawn:config->spawns){auto r=row(Category::Respawn,spawn.actor,"Respawn destination "+std::to_string(spawn.id));const auto p=vec(spawn.position);uint32_t color=cyan;
            if(cp->actors.contains(spawn.actor)){const auto state=detCharacterRespawnState2D(*cp,spawn.actor,*config);if(state.spawn==spawn.id)color=state.result==DetRespawnResult2D::Accepted?green:state.result==DetRespawnResult2D::None?yellow:red;
                r.details="Recorded result "+std::to_string(int(state.result))+" | last spawn "+std::to_string(state.spawn)+" | result nextTick "+std::to_string(state.resultTick)+"\nResult describes the real relocation attempt, not a live debug clearance test.";
                const auto schema=detCharacterStateSchema2D(config->character);const auto& words=cp->actors.at(spawn.actor).blocks.at(config->character.stateSchema);
                if(readDetState<bool>(schema,words,DetCharacterStandingCaptured)){const auto half=vec(readDetState<DV>(schema,words,DetCharacterStandingHalf)),offset=vec(readDetState<DV>(schema,words,DetCharacterStandingOffset));box(r,p+offset-half,p+offset+half,color);}}
            marker(r,p,color,.25f);out.append(std::move(r));
        }
        for(const auto& config:policies->respawnFlows){for(const auto& binding:config->bindings){const auto state=detCharacterRespawnFlowState2D(*cp,binding.actor,*config);auto r=row(Category::Respawn,binding.actor,"Checkpoint / death routing");
            r.details="Active spawn "+std::to_string(state.activeSpawn)+" | checkpoint count "+std::to_string(state.checkpointCount)+" | last checkpoint region "+std::to_string(state.lastCheckpointRegion)+"\nDeaths "+std::to_string(state.deathCount)+" | requests "+std::to_string(state.requestCount)+" | kind "+std::to_string(int(state.lastRequestKind))+" | queued request code "+std::to_string(int(state.lastRequestCode))+"\nRouting permission and final relocation outcome are separate observations.";marker(r,r.anchor);out.append(std::move(r));}
            for(const auto& region:config->regions){auto r=row(Category::Respawn,region.actor,"Checkpoint / death region "+std::to_string(region.id));r.otherEntityId=entityId(region.trigger);r.details="Region kind "+std::to_string(int(region.kind))+" | trigger "+std::to_string(region.trigger);if(cp->actors.contains(region.trigger))marker(r,position(region.trigger),region.kind==DetRespawnFlowRegionKind2D::Death?red:green,.25f);out.append(std::move(r));}
        }
    }
    }catch(const std::exception& error){Record r;r.category=enabled(categories,Category::DeterministicPhysics)?Category::DeterministicPhysics:enabled(categories,Category::PathInteraction)?Category::PathInteraction:Category::Respawn;r.label="Deterministic observation unavailable";r.details=error.what();out.append(std::move(r));}
#endif
    return out.finish();
}
}
