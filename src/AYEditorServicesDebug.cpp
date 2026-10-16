#include "EditorDebugVisualization.h"
#include "EditorDebugPrimitives.h"
#include <AYEntity.h>
#include <AYEntity/CameraRuntime.h>
#include <AYEntity/components/CameraControllerComponent.h>
#include <AYEntity/components/NetworkComponent.h>
#include <AYApplication.h>
#include <AYApplication/IEngineHost.h>
#include <AYDevice/DeviceManager.h>
#include <AYResource/ResourceManager.h>
#include <AYUI/UIManager.h>
#include <AYNetwork.h>
#include <AYNetwork/Profiler/ProfilerSnapshot.h>
#include <sstream>
#include <algorithm>
#include <cmath>
#if defined(AYEDITOR_HAS_STATS)
#include <AYStats/EntityIntegration.h>
#include <AYStats/Calculation.h>
#endif
#if defined(AYEDITOR_HAS_SEQUENCE)
#include <AYSequence/SceneSequenceSession.h>
#endif
#if defined(AYEDITOR_HAS_DETERMINISTIC_DEBUG)
#include <AYEntity/DeterministicHost.h>
#endif
namespace ayt::editor {
using namespace debug_detail;
void clearServiceDebugHistory(ayt::entity::World* world,int category) {
    auto selected=[&](Category value){return category<0 || category==int(value);};
#if defined(AYEDITOR_HAS_STATS)
    if(selected(Category::Stats)){ayt::stats::clearCalculationDebugHistory();if(category<0)ayt::stats::setCalculationDebugCaptureEnabled(false);}
#endif
#if defined(AYEDITOR_HAS_SEQUENCE)
    if(selected(Category::Sequence)){ayt::sequence::SceneSequenceSession::clearDebugHistory();if(category<0)ayt::sequence::SceneSequenceSession::setDebugCaptureEnabled(false);}
#endif
    if(selected(Category::Resource))if(auto* host=ayt::app::currentEngineHost())if(auto* resources=host->resources()){resources->clearDebugHistory();if(category<0)resources->setDebugCaptureEnabled(false);}
    if(selected(Category::Network))if(auto* network=ayt::net::findRegisteredNetworkSubSystem())if(auto* replication=network->getReplicationManager()){replication->clearDebugHistory();if(category<0)replication->setDebugCaptureEnabled(false);}
    if(selected(Category::CameraBehavior) && world)if(auto* runtime=dynamic_cast<ayt::entity::CameraRuntimeSystem*>(world->findSystemByName("CameraRuntimeSystem"))){runtime->clearDebugHistory();if(category<0)runtime->setDebugCaptureEnabled(false);}
}

std::vector<Record> editorServiceDebugRecords(ayt::entity::World& world,ayt::ui::UIManager& ui,uint32_t categories) {
    Collector out;out.categories=categories;auto* host=ayt::app::currentEngineHost();
    auto entityRow=[&](Category category,ayt::entity::Entity& entity,const std::string& label){Record r;r.category=category;r.entityId=entity.getId();r.label=label+" | "+(entity.getName()?entity.getName():"");if(auto* pose=entity.getComponent<ayt::entity::Transform>())marker(r,pose->position);return r;};
    if(auto* runtime=dynamic_cast<ayt::entity::CameraRuntimeSystem*>(world.findSystemByName("CameraRuntimeSystem"))) {
        runtime->setDebugCaptureEnabled(enabled(categories,Category::CameraBehavior));
        if(enabled(categories,Category::CameraBehavior)) {
            const auto snapshot=runtime->debugSnapshot();
            for(const auto& probe:snapshot.collisions){Record r;r.category=Category::CameraBehavior;r.entityId=probe.entityId;r.label="Actual camera collision probe";
                r.details="Radius "+std::to_string(probe.radius)+" | allowed distance "+std::to_string(probe.allowed)+" | valid result "+std::to_string(probe.valid)+"\nCaptured from the real adapter call; debug does not issue a new sweep.";
                r.lines.push_back({probe.anchor,probe.desiredEye,cyan});r.lines.push_back({probe.anchor,probe.resolvedEye,probe.valid?green:yellow});marker(r,probe.resolvedEye,probe.valid?(probe.resolvedEye-probe.desiredEye).length()>.001f?red:green:yellow);out.append(std::move(r));}
            for(const auto& view:snapshot.views){Record r;r.category=Category::CameraBehavior;r.entityId=view.frame.sourceEntityId;r.queryId=uint32_t(view.viewport);r.label="Viewport camera transition";
                r.details="Viewport "+std::to_string(view.viewport)+" | 2D "+std::to_string(view.twoD)+" | raw transition progress "+std::to_string(view.progress)+" | duration "+std::to_string(view.duration)+" s | source identity "+std::to_string(view.frame.identity);
                marker(r,view.frame.position);out.append(std::move(r));}
        }
    }
    for(auto* entity:world.getAllEntities())if(entity) {
        auto* pose=entity->getComponent<ayt::entity::Transform>();
        if(enabled(categories,Category::CameraBehavior))if(auto* controller=entity->getComponent<ayt::entity::CameraControllerComponent>();controller && pose){
            auto r=entityRow(Category::CameraBehavior,*entity,"Camera follow / dead zone / shake");
            auto* target=controller->targetName.empty()?nullptr:world.findEntity(controller->targetName.c_str());auto* targetPose=target?target->getComponent<ayt::entity::Transform>():nullptr;
            if(targetPose){r.otherEntityId=target->getId();r.lines.push_back({pose->position,targetPose->position,cyan});}
            if(controller->hasCollisionAnchor){marker(r,controller->collisionAnchor,yellow);r.lines.push_back({pose->position,controller->collisionAnchor,yellow});}
            if(controller->twoD){const V half{std::max(0.f,controller->deadZone.x),std::max(0.f,controller->deadZone.y),0};box(r,pose->position-half,pose->position+half,cyan);}
            if(controller->boundsEnabled)box(r,controller->boundsMin,controller->boundsMax,yellow);
            r.lines.push_back({pose->position,pose->position+controller->shakePosition,red});
            r.details="Mode "+std::to_string(controller->mode)+" | target "+controller->targetName+" | target resolved "+std::to_string(targetPose!=nullptr)+"\nDead zone "+std::to_string(controller->deadZone.x)+","+std::to_string(controller->deadZone.y)+" | look-ahead "+std::to_string(controller->lookAheadSeconds)+" s\nDamping position/rotation "+std::to_string(controller->positionDamping)+" / "+std::to_string(controller->rotationDamping)+"\nShake elapsed "+std::to_string(controller->shakeElapsed)+" / "+std::to_string(controller->shakeDuration)+" s | frequency "+std::to_string(controller->shakeFrequency);
            out.append(std::move(r));
        }
#if defined(AYEDITOR_HAS_STATS)
        if(enabled(categories,Category::Stats))if(auto* component=entity->getComponent<ayt::stats::StatsComponent>()) {
            if(out.counts[size_t(Category::Stats)]>=80){++out.omitted[size_t(Category::Stats)];continue;}
            const auto* state=component->state();if(!state){auto r=entityRow(Category::Stats,*entity,"Stats unavailable");r.details=component->lastError()?component->lastError()->message:"No live state (debug does not hydrate authoring).";out.append(std::move(r));continue;}
            const auto definitions=state->schema()->definitions();const auto rows=state->rows();size_t shown=0;
            for(size_t i=0;i<definitions.size();++i){if(shown++==128)break;const auto& definition=definitions[i];const auto& value=rows[i];auto r=entityRow(Category::Stats,*entity,"Attribute "+definition.name);r.queryId=uint32_t(i);
                std::ostringstream text;text<<"Version "<<component->version()<<" | authority "<<component->authority()<<" | base "<<value.base<<" | effective "<<value.effective<<" | current "<<value.current<<" | capacity "<<value.capacity
                    <<"\nMinimum "<<definition.minimum<<" | maximum "<<definition.maximum<<" | capacity source "<<definition.capacityFrom<<" | base source "<<definition.baseFrom;
                size_t modifierCount=0;for(const auto& modifier:state->modifiers())if(modifier.target==definition.id){if(modifierCount++==64){text<<"\nMore modifiers omitted";break;}text<<"\nModifier "<<modifier.handle<<" effect "<<modifier.effect<<" rule "<<modifier.rule<<" stage "<<modifier.stage<<" zone "<<modifier.zone<<" value "<<modifier.value<<" priority "<<modifier.priority;}
                for(const auto& zone:definition.zones)text<<"\nZone "<<zone.id<<" operation "<<int(zone.op);r.details=text.str();if(!out.append(std::move(r)) || out.counts[size_t(Category::Stats)]>=80)break;}
        }
#endif
    }
#if defined(AYEDITOR_HAS_STATS)
    ayt::stats::setCalculationDebugCaptureEnabled(enabled(categories,Category::Stats));
    const auto calculations=enabled(categories,Category::Stats)?ayt::stats::calculationDebugSnapshot():std::vector<ayt::stats::CalculationDebugRecord>{};
    for(auto observed=calculations.rbegin();observed!=calculations.rend();++observed) {const auto& calculation=*observed;
        Record r;r.category=Category::Stats;r.queryId=uint32_t(calculation.sequence);r.transientEvent=true;r.label="Actual calculation "+std::to_string(calculation.context.calculation);
        std::ostringstream text;text<<"Thread-local observation "<<calculation.sequence<<" | tick "<<calculation.context.tick<<" | hit "<<calculation.context.hit<<" | result "<<(calculation.error.empty()?std::to_string(calculation.result):"unavailable")
            <<"\nPure calculation observation; settlement/permission is separate. No re-evaluation.\nInputs: ";for(const auto value:calculation.inputs)text<<value<<" ";
        static const char* operationNames[]={"Input","Constant","Add","Subtract","Multiply","Divide","MultiplyRate","Minimum","Maximum","Clamp","Greater","Select","Table","Random","Extension"};
        for(const auto& stage:calculation.stages)text<<"\nNode "<<stage.node<<" = "<<operationNames[size_t(stage.operation)]<<"("<<stage.a<<","<<stage.b<<","<<stage.c<<") -> "<<stage.value<<" | binding "<<stage.binding<<" parameter "<<stage.parameter<<(stage.extension.empty()?"":" extension "+stage.extension);
        if(!calculation.error.empty())text<<"\nError "<<calculation.error;if(calculation.truncated)text<<"\nNode/input display capped at256";r.details=text.str();out.append(std::move(r));
    }
#endif
    ui.setDebugCaptureEnabled(enabled(categories,Category::UI));
    if(enabled(categories,Category::UI)) {
        const auto snapshot=ui.debugSnapshot();
        for(const auto& widget:snapshot.widgets){Record r;r.category=Category::UI;r.label="UI "+widget.id;
            r.details="Parent "+widget.parent+" | focus "+std::to_string(widget.focused)+" | hover "+std::to_string(widget.hovered)+" | capture "+std::to_string(widget.captured)+"\nLogical bounds "+std::to_string(widget.bounds.minX)+","+std::to_string(widget.bounds.minY)+" .. "+std::to_string(widget.bounds.maxX)+","+std::to_string(widget.bounds.maxY)+"\nBlue is the container client guide; yellow is the last actual backend rectangular clip.";
            screenRect(r,widget.bounds,widget.captured?red:widget.focused?yellow:widget.hovered?green:0xff808080);screenRect(r,widget.client,cyan);if(widget.paintClip.valid){screenRect(r,widget.paintClip.rectangle,yellow);r.details+="\nLast paint path-clip depth "+std::to_string(widget.paintClip.pathDepth);}else r.details+="\nActual paint clip unavailable before capture / unsupported backend";out.append(std::move(r));}
        Record r;r.category=Category::UI;r.label="UI pointer / routing budget";r.details="Pointer known "+std::to_string(snapshot.pointerKnown)+" | x/y "+std::to_string(snapshot.pointer.x)+","+std::to_string(snapshot.pointer.y)+" | omitted widgets "+std::to_string(snapshot.dropped);out.append(std::move(r));
    }
    if(enabled(categories,Category::Input) && host)if(auto* device=ayt::app::deviceManager(*host);device && device->isInitialized()) {
        const auto snapshot=device->mapping().debugSnapshot();
        size_t actionRows=0;for(const auto& action:snapshot.actions){if(actionRows++==64)break;Record r;r.category=Category::Input;r.label="Action "+action.name;
            r.details="Frame held/pressed/released "+std::to_string(action.held)+" / "+std::to_string(action.pressed)+" / "+std::to_string(action.released)+"\nTick held/pressed/released "+std::to_string(action.tickHeld)+" / "+std::to_string(action.tickPressed)+" / "+std::to_string(action.tickReleased)+" | sim tick "+std::to_string(snapshot.inputTick)+"\nBound keys/mouse/gamepad "+std::to_string(action.keys)+" / "+std::to_string(action.mouseButtons)+" / "+std::to_string(action.gamepadButtons)+" | UI capturing "+std::to_string(ui.isCapturing());out.append(std::move(r));}
        size_t axisIndex=0;for(const auto& axis:snapshot.axes){if(axisIndex==48)break;Record r;r.category=Category::Input;r.label="Axis "+axis.name;r.details="Frame value "+std::to_string(axis.value)+" | fixed-tick value "+std::to_string(axis.tickValue)+" | sim tick "+std::to_string(snapshot.inputTick)+"\nKeyboard/gamepad sources "+std::to_string(axis.keyboardSources)+" / "+std::to_string(axis.gamepadSources);
            if(ui.root() && axisIndex<12){const float x=std::max(140.f,ui.root()->getSize().x-160),y=55.f+float(axisIndex)*18;
                r.screenLines.push_back({{x-100,y,0},{x+100,y,0},0xff808080});r.screenLines.push_back({{x,y,0},{x+std::clamp(axis.value,-1.f,1.f)*100,y,0},std::fabs(axis.value)>.001f?red:green});}
            ++axisIndex;out.append(std::move(r));}
        Record summary;summary.category=Category::Input;summary.label="Input observation budget";summary.details="Snapshot actions/axes "+std::to_string(snapshot.actions.size())+" / "+std::to_string(snapshot.axes.size())+" | source omitted "+std::to_string(snapshot.dropped)+"\nEditor displays up to64 actions and48 axes; the first12 axes have screen meters.";out.append(std::move(summary));
    }
    if(auto* network=ayt::net::findRegisteredNetworkSubSystem())if(auto* replication=network->getReplicationManager()) {
        replication->setDebugCaptureEnabled(enabled(categories,Category::Network));
        if(enabled(categories,Category::Network)) {
            const auto snapshot=replication->debugSnapshot();Record summary;summary.category=Category::Network;summary.label="Replication tick / budget";
            summary.details="Authority "+std::to_string(snapshot.authority)+" | server tick "+std::to_string(snapshot.serverTick)+" | sent bytes "+std::to_string(snapshot.tick.sentBytes)+" | frames "+std::to_string(snapshot.tick.sentFrames)+"\nDeferred budget/LOD/backpressure "+std::to_string(snapshot.tick.deferredByBudget)+" / "+std::to_string(snapshot.tick.deferredByLod)+" / "+std::to_string(snapshot.tick.deferredByBackpressure)+" | source omitted objects "+std::to_string(snapshot.dropped)+" | editor omitted objects "+std::to_string(snapshot.objects.size()>64?snapshot.objects.size()-64:0);out.append(std::move(summary));
            size_t objectRows=0;for(const auto& object:snapshot.objects){if(objectRows++==64)break;Record r;r.category=Category::Network;r.queryId=object.netId;r.label="Network object "+std::to_string(object.netId);
                for(auto* e:world.getAllEntities())if(e)if(auto* component=e->getComponent<ayt::entity::NetworkComponent>();component && component->getNetId()==object.netId){r.entityId=e->getId();break;}
                auto vec=[](ayt::net::NetVec3 v){return V{v.x,v.y,v.z};};if(object.hasAuthority)marker(r,vec(object.authority),red,.18f);if(object.hasPredicted)marker(r,vec(object.predicted),green,.14f);if(object.hasInterpolated)marker(r,vec(object.interpolated),cyan,.1f);
                if(object.hasAuthority && object.hasPredicted)r.lines.push_back({vec(object.authority),vec(object.predicted),yellow});
                r.details="Proxy kind "+std::to_string(int(object.proxy))+" | owner connection "+std::to_string(object.ownerConnection)+" | dirty fields "+(object.dirtyFields==SIZE_MAX?std::string("unavailable"):std::to_string(object.dirtyFields))+"\nPredicted input "+std::to_string(object.predictedInput)+" | interpolation records "+std::to_string(object.interpolationRecords)+" | last correction distance "+std::to_string(object.lastCorrectionDistance)+"\nAuthority/predicted/interpolated pose available "+std::to_string(object.hasAuthority)+" / "+std::to_string(object.hasPredicted)+" / "+std::to_string(object.hasInterpolated)+"\nPose requires an exact typed position/location/translation field; unavailable data is not inferred.";out.append(std::move(r));}
            std::vector<ayt::net::ProfilerSnapshot> connections;network->getProfilerSnapshots(connections);size_t shown=0;
            for(const auto& connection:connections){if(shown++==32)break;Record r;r.category=Category::Network;r.label="Connection "+std::to_string(connection.netId);r.queryId=connection.netId;r.details="Ping "+std::to_string(connection.live.pingMs)+" ms | send/receive bytes per second "+std::to_string(connection.live.outBytesPerSec)+" / "+std::to_string(connection.live.inBytesPerSec)+"\nSend queue bytes "+std::to_string(connection.live.sendQueueBytes)+" | reliable pending "+std::to_string(connection.live.pendingReliable)+" | ack pending "+std::to_string(connection.live.ackPending);out.append(std::move(r));}
        }
    }
#if defined(AYEDITOR_HAS_DETERMINISTIC_DEBUG)
    if(enabled(categories,Category::Network) && host)if(auto* controller=ayt::entity::deterministicHost(*host))if(controller->session()) {
        const auto snapshot=controller->rollbackDiagnostics();Record r;r.category=Category::Network;r.label="Deterministic network / rollback";
        std::ostringstream text;text<<"Epoch "<<controller->networkEpoch()<<" | waiting "<<controller->networkWaiting()<<" | synchronized "<<controller->networkSynchronized()<<"\nHead/confirmed/verified/oldest "<<snapshot.head<<" / "<<snapshot.confirmed<<" / "<<snapshot.verified<<" / "<<snapshot.oldest<<"\nRollbacks "<<snapshot.rollbacks<<" | replayed ticks "<<snapshot.replayedTicks<<" | last/max depth "<<snapshot.lastDepth<<" / "<<snapshot.maxDepth<<" | predicted ticks "<<snapshot.predictedTicks<<" | buffered bytes "<<snapshot.bufferedBytes<<"\nMissing members: ";for(auto member:controller->networkMissing())text<<member<<" ";
        if(const auto& difference=controller->difference())text<<"\nHash/state difference tick "<<difference->tick<<" entity "<<difference->entity<<" field "<<difference->field<<" section "<<difference->section;
        if(!controller->error().empty())text<<"\nHost diagnostic: "<<controller->error();r.details=text.str();out.append(std::move(r));
    }
#endif
#if defined(AYEDITOR_HAS_SEQUENCE)
    ayt::sequence::SceneSequenceSession::setDebugCaptureEnabled(enabled(categories,Category::Sequence));
    if(enabled(categories,Category::Sequence))for(const auto& session:ayt::sequence::SceneSequenceSession::debugSnapshots(world)) {
        Record r;r.category=Category::Sequence;r.queryId=uint32_t(session.session);r.label="Sequence "+session.sequence;r.details="Session "+std::to_string(session.session)+" | state "+std::to_string(int(session.state))+" | time "+std::to_string(session.position)+" / "+std::to_string(session.duration)+" s\n"+session.error;out.append(std::move(r));
        for(const auto& binding:session.bindings){Record row;row.category=Category::Sequence;row.entityId=binding.entityId;row.queryId=uint32_t(session.session);row.label="Sequence binding "+binding.name;row.details="Transform owned "+std::to_string(binding.ownsTransform)+" | position "+std::to_string(session.position)+" s";
            if(auto* entity=world.findEntity(binding.entityId))if(auto* pose=entity->getComponent<ayt::entity::Transform>())marker(row,pose->position);
            for(const auto& section:binding.activeSections)row.details+="\nActive section "+section;out.append(std::move(row));}
        for(auto observed=session.events.rbegin();observed!=session.events.rend();++observed){const auto& event=*observed;Record row;row.category=Category::Sequence;row.queryId=uint32_t(event.batch);row.transientEvent=true;row.label="Sequence event "+event.event.id+" | session "+std::to_string(session.session)+(event.delivered?" | delivered":" | crossed");row.details="Event "+event.event.name+" | source time "+std::to_string(event.event.time)+" | batch "+std::to_string(event.batch)+" | generation "+std::to_string(event.generation)+" | delivered "+std::to_string(event.delivered)+"\n"+event.event.payload;out.append(std::move(row));}
    }
#endif
    if(host && host->resources()) {
        auto* resources=host->resources();resources->setDebugCaptureEnabled(enabled(categories,Category::Resource));
        if(enabled(categories,Category::Resource)){const auto snapshot=resources->debugSnapshot();Record summary;summary.category=Category::Resource;summary.label="Resource cache / loading";
            summary.details="CPU cache bytes "+std::to_string(snapshot.cache.memoryBytes)+" / "+std::to_string(snapshot.cache.memoryBudget)+" | strong/weak/grace "+std::to_string(snapshot.cache.strongCount)+" / "+std::to_string(snapshot.cache.weakCount)+" / "+std::to_string(snapshot.cache.graceCount)+"\nHits/misses/resurrections "+std::to_string(snapshot.cache.hitCount)+" / "+std::to_string(snapshot.cache.missCount)+" / "+std::to_string(snapshot.cache.resurrectCount)+" | source omitted rows "+std::to_string(snapshot.dropped)+"\nEditor quotas:80 loads,24 cache-only rows,16 reload observations.\nCPU cache residency and GPU ownership are separate.";out.append(std::move(summary));
            static const char* states[]={"Not loaded","Loading","Ready","Failed"};
            size_t loadRows=0;for(const auto& load:snapshot.loads){if(loadRows++==80)break;Record r;r.category=Category::Resource;r.label="Resource "+load.path;r.details=std::string(states[size_t(load.state)])+" | cached on failure "+std::to_string(load.cachedOnFailure)+" | progress "+(load.progress<0?std::string("unavailable"):std::to_string(load.progress*100)+"%")+"\nFailure can retain a fallback resource; a cached pointer does not imply successful loading.";
                for(const auto& resident:snapshot.resident)if(resident.path==load.path){r.details+="\nResident type "+resident.type+" | bytes "+std::to_string(resident.sizeInBytes);break;}out.append(std::move(r));}
            size_t cacheRows=0;for(const auto& resident:snapshot.resident){if(std::any_of(snapshot.loads.begin(),snapshot.loads.end(),[&](const auto& load){return load.path==resident.path;}))continue;if(cacheRows++==24)break;Record r;r.category=Category::Resource;r.label="Cached resource "+resident.path;r.details="Resident type "+resident.type+" | bytes "+std::to_string(resident.sizeInBytes)+"\nNo matching load-state row; residency does not establish load success.";out.append(std::move(r));}
            size_t index=0;for(auto observed=snapshot.reloads.rbegin();observed!=snapshot.reloads.rend();++observed){const auto& reload=*observed;if(index++==16)break;Record r;r.category=Category::Resource;r.queryId=uint32_t(reload.sequence);r.transientEvent=true;r.label="Observed hot reload "+reload.path;r.details="Independent bounded reload history; gameplay hot-reload callback remains installed.";out.append(std::move(r));}
        }
    }
    return out.finish();
}
}
