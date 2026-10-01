#include <AYEditor/EditorParticleDocument.h>
#include <AYParticle/EffectResource.h>
#include <AYResource/ResourceManager.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
namespace ayt::editor {
bool EditorParticleDocument::initialize(const EditorOpenRequest& request,std::string& error) {
    particle::EffectAsset next;
    if(!request.resourcePath.empty()) {
        if(!particle::loadEffectAsset(request.resourcePath,next,&error)) return false;
    } else next=particle::combinedExplosion();
    _asset=_saved=next; _path=request.resourcePath;
    _previewSettings.scene3D=std::any_of(next.emitters.begin(),next.emitters.end(),[](const auto& layer){return layer.effect.dimension==particle::Dimension::ThreeD;});
    _title=_path.empty()?"Particle Effect":std::filesystem::path(_path).filename().string();
    _history={_asset}; _cursor=0; resetPreview(); error.clear(); return true;
}
void EditorParticleDocument::resetPreview() {
    std::unordered_set<std::string> keys;
    for(size_t i=0;i<_asset.emitters.size();++i)keys.insert(previewLayerKey(i));
    if(!keys.contains(_previewSettings.solo))_previewSettings.solo.clear();
    std::erase_if(_previewSettings.hidden,[&](const auto& key){return !keys.contains(key);});
    if(_runtimeGpu) _preview.reset();
    else {
        auto previewAsset=_asset;previewAsset.seed=previewSeed();
        _preview=std::make_unique<particle::EffectInstance>(previewAsset);
        _preview->play(); _preview->update(0);
    }
    _playing=false; _simulationMs=0; _previewTime=0;
    _previewStopped=false; _replayPending=false; ++_previewEpoch;
}
bool EditorParticleDocument::replaceAsset(const particle::EffectAsset& next,std::string* error) {
    if(!next.validate(error)) return false;
    _diagnostic.clear();
    if(next==_asset) return true;
    _history.resize(_cursor+1); _history.push_back(next); ++_cursor;
    if(_history.size()>128) { _history.erase(_history.begin()); --_cursor; }
    _asset=next; ++_revision; resetPreview(); return true;
}
bool EditorParticleDocument::undo() {
    if(!_cursor) return false; _asset=_history[--_cursor]; ++_revision; resetPreview(); return true;
}
bool EditorParticleDocument::redo() {
    if(_cursor+1>=_history.size()) return false; _asset=_history[++_cursor]; ++_revision; resetPreview(); return true;
}
bool EditorParticleDocument::save(std::string* error) {
    if(_path.empty()) {
        const auto chosen=_savePath?_savePath(false):std::string{};
        if(chosen.empty()) { if(error) *error="Choose a particle effect save path."; return false; }
        return saveAs(chosen,error);
    }
    return saveAs(_path,error);
}
bool EditorParticleDocument::saveAs(const std::string& path,std::string* error) {
    if(!particle::saveEffectAsset(path,_asset,error)) return false;
    _path=path; _title=std::filesystem::path(path).filename().string(); _saved=_asset; ++_revision;
    particle::registerEffectResourceLoader();
    auto& resources=resource::ResourceManager::instance();
    if(resources.isLoaded(path) || resources.hasLoadFailed(path)) (void)particle::reloadEffectResource(path);
    return true;
}
bool EditorParticleDocument::reload(std::string* error) {
    particle::EffectAsset next;
    if(!particle::loadEffectAsset(_path,next,error)) return false;
    _asset=_saved=next; _history={next}; _cursor=0; ++_revision; resetPreview(); return true;
}
bool EditorParticleDocument::writeRecoveryCopy(const std::string& path,std::string* error) const {
    return particle::saveEffectAsset(path,_asset,error);
}
void EditorParticleDocument::play() {
    if(!_preview&&!_runtimeGpu) return;
    if(_runtimeGpu?_previewStopped:_preview->finished()) resetPreview();
    _playing=true;
}
void EditorParticleDocument::restart() { resetPreview(); _playing=true; }
void EditorParticleDocument::stop() { resetPreview(); if(_preview)_preview->stop(true); _previewStopped=true; }
void EditorParticleDocument::step() {
    _playing=false;
    if(_runtimeGpu) { if(_previewStopped) resetPreview(); _previewTime+=1.0/60; return; }
    if(_preview && _preview->finished()) resetPreview();
    if(_preview) {
        const auto start=std::chrono::steady_clock::now(); _preview->update(1.0f/60);
        _simulationMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    }
}
void EditorParticleDocument::tick(float dt) {
    if(!_playing || (!_preview&&!_runtimeGpu) || !std::isfinite(dt) || dt<0) return;
    if(_runtimeGpu) { if(!_replayPending) _previewTime+=std::min(dt,.25f); return; }
    const auto start=std::chrono::steady_clock::now();
    _preview->update(dt);
    _simulationMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(_preview->finished()) _playing=false;
}
bool EditorParticleDocument::seek(float seconds) {
    if(!std::isfinite(seconds)||seconds<0||seconds>60)return false;
    resetPreview();if(_runtimeGpu) { _previewTime=seconds; return true; }
    const int steps=int(std::floor(seconds*60));
    for(int i=0;i<steps;++i)_preview->update(1.f/60);
    const float rest=seconds-float(steps)/60;if(rest>0)_preview->update(rest);
    return true;
}
void EditorParticleDocument::setPreviewSeed(uint32_t seed) {_previewSeed=seed;_seedOverride=true;resetPreview();}
void EditorParticleDocument::useAssetSeed() {_seedOverride=false;resetPreview();}
std::string EditorParticleDocument::previewLayerKey(size_t index) const {
    const auto& e=_asset.emitters.at(index);
    return e.id?"id:"+std::to_string(e.id):"name:"+e.name;
}
bool EditorParticleDocument::previewLayerVisible(size_t index) const {
    const auto key=previewLayerKey(index);
    return !_previewSettings.hidden.contains(key)
        && (_previewSettings.solo.empty()||_previewSettings.solo==key);
}
void EditorParticleDocument::togglePreviewLayerHidden(size_t index) {
    const auto key=previewLayerKey(index);
    if(!_previewSettings.hidden.erase(key))_previewSettings.hidden.insert(key);
}
void EditorParticleDocument::togglePreviewLayerSolo(size_t index) {
    const auto key=previewLayerKey(index);
    _previewSettings.solo=_previewSettings.solo==key?std::string{}:key;
}
void EditorParticleDocument::setRuntimeGpuActive(bool active) {
    if(active==_runtimeGpu)return;
    const auto time=previewTime(); const bool playing=_playing,stopped=_previewStopped;
    _runtimeGpu=active;
    resetPreview();
    if(active) _previewTime=time;
    else if(!stopped) {
        // Bounded authoring replay. Long-running loops can restore their current cycle.
        const double replay=time<=60?time:(_asset.looping?std::fmod(time,double(_asset.duration)):60.0);
        const int steps=int(std::floor(std::min(replay,60.0)*60));
        for(int i=0;i<steps;++i)_preview->update(1.f/60);
        const float rest=float(std::min(replay,60.0)-double(steps)/60);
        if(rest>0)_preview->update(rest);
    }
    if(stopped&&_preview)_preview->stop(true);
    _previewStopped=stopped; _playing=playing;
}
bool EditorParticleDocument::handlesCommand(const std::string& id) const {
    return id=="file.save" || id=="file.save_as" || id=="file.reload" || id=="edit.undo" || id=="edit.redo" || id=="particle.play" || id=="particle.pause"
        || id=="particle.restart" || id=="particle.stop" || id=="particle.step";
}
bool EditorParticleDocument::canExecuteCommand(const std::string& id) const {
    if(id=="file.save") return isDirty() && (!_path.empty() || bool(_savePath));
    if(id=="file.save_as") return bool(_savePath);
    if(id=="file.reload") return !_path.empty() && !isDirty();
    if(id=="edit.undo") return _cursor>0;
    if(id=="edit.redo") return _cursor+1<_history.size();
    if(id=="particle.pause") return _playing;
    return handlesCommand(id) && (_preview!=nullptr||_runtimeGpu);
}
bool EditorParticleDocument::executeCommand(const std::string& id) {
    if(!canExecuteCommand(id)) return false;
    _diagnostic.clear();
    if(id=="file.save") return save(&_diagnostic);
    if(id=="file.reload") return reload(&_diagnostic);
    if(id=="file.save_as") {
        const auto path=_savePath(true); return !path.empty() && saveAs(path,&_diagnostic);
    }
    if(id=="edit.undo") return undo();
    if(id=="edit.redo") return redo();
    if(id=="particle.play") play();
    else if(id=="particle.pause") pause();
    else if(id=="particle.restart") restart();
    else if(id=="particle.stop") stop();
    else if(id=="particle.step") step();
    return true;
}
}