#pragma once
#include <AYEditor/EditorParticleDocument.h>
#include <AYUI/Authoring/TimelineModel.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ayt::editor {
// Adapter for the shared authoring CurveCanvas. A gesture owns a private draft;
// only its final validated value enters document history. Cancel discards it.
class ParticleCurveSource final : public ui::authoring::ICurveEditorSource {
public:
    ParticleCurveSource(std::shared_ptr<EditorParticleDocument> document,size_t emitter,int kind)
        :_document(std::move(document)),_emitter(emitter),_kind(kind) {}
    uint64_t revision() const noexcept override { return _document->revision()+_serial; }
    double durationSeconds() const noexcept override { return 1; }
    double positionSeconds() const noexcept override { return 0; }
    bool seek(double) override { return false; }
    double snapTime(const std::string&,double t) const override { return std::clamp(t,0.0,1.0); }
    bool editing() const noexcept { return _editing; }
    bool beginEdit(const std::string&) override {
        if(_editing || _emitter>=_document->asset().emitters.size() || empty()) return false;
        _draft=_document->asset(); _baseRevision=_document->revision();
        _editing=true; _document->pause(); return true;
    }
    bool endEdit(bool cancel) override {
        if(!_editing) return true;
        if(!cancel && _baseRevision==_document->revision() && !_document->replaceAsset(_draft)) return false;
        _editing=false; ++_serial; return true;
    }
    bool updateKey(std::string& id,double time,const std::vector<float>& values) override {
        if(!_editing || _baseRevision!=_document->revision() || !std::isfinite(time)) return false;
        size_t index=0;
        try { index=std::stoull(id); } catch(...) { return false; }
        auto next=_draft; auto& e=next.emitters[_emitter].effect;
        const auto track=curveTrack("lifetime");
        if(index>=track->keys.size() || values.size()!=(_kind==2?4u:1u)) return false;
        float t=static_cast<float>(std::clamp(time,0.0,1.0));
        if(index==0) t=0;
        else if(index+1==track->keys.size()) t=1;
        else t=std::clamp(t,std::nextafter(float(track->keys[index-1].timeSeconds),1.0f),
                           std::nextafter(float(track->keys[index+1].timeSeconds),0.0f));
        if(_kind==2) e.colorGradient[index]={t,{std::max(0.0f,values[0]),std::max(0.0f,values[1]),std::max(0.0f,values[2]),std::clamp(values[3],0.0f,1.0f)}};
        else {
            auto& keys=scalarKeys(e);
            keys[index]={t,scalarValue(values[0])};
        }
        if(!next.validate()) return false;
        _draft=std::move(next); ++_serial; return true;
    }
    bool moveTimelineKey(std::string&,std::string& id,double t) override {
        const auto track=curveTrack("lifetime");
        for(const auto& k:track->keys) if(k.id==id) return updateKey(id,t,k.values);
        return false;
    }
    bool transformKeys(std::vector<std::string>& ids,double deltaTime,size_t component,float deltaValue) override {
        if(!_editing || _baseRevision!=_document->revision() || !std::isfinite(deltaTime) || !std::isfinite(deltaValue)) return false;
        const auto track=curveTrack("lifetime");
        if(ids.empty() || component>=(_kind==2?4u:1u)) return false;
        std::vector<size_t> selected;
        for(const auto& id:ids) {
            size_t index=0; try { index=std::stoull(id); } catch(...) { return false; }
            if(index>=track->keys.size()) return false;
            selected.push_back(index);
        }
        auto next=_draft; auto& e=next.emitters[_emitter].effect;
        for(const auto index:selected) {
            const auto& key=track->keys[index];
            // Keep endpoints fixed; validation below rejects crossing or duplicates
            // for the entire selected group rather than partially applying it.
            const float t=index==0?0:index+1==track->keys.size()?1:float(key.timeSeconds+deltaTime);
            if(_kind==2) {
                auto& k=e.colorGradient[index]; k.time=t;
                const float v=component==3?std::clamp(key.values[component]+deltaValue,0.0f,1.0f):std::max(0.0f,key.values[component]+deltaValue);
                if(component==0)k.color.r=v; else if(component==1)k.color.g=v; else if(component==2)k.color.b=v; else k.color.a=v;
            } else {
                auto& k=scalarKeys(e)[index]; k.time=t;
                k.value=scalarValue(key.values[0]+deltaValue);
            }
        }
        if(!next.validate()) return false;
        _draft=std::move(next); ++_serial; return true;
    }
    std::shared_ptr<const ui::authoring::TimelineSnapshot> timelineSnapshot() const override {
        auto result=std::make_shared<ui::authoring::TimelineSnapshot>();
        result->tracks.push_back({"lifetime","Lifetime",ui::authoring::TimelineTrackKind::Value});
        for(const auto& k:curveTrack("lifetime")->keys) result->keys.push_back({k.id,"lifetime",k.timeSeconds});
        return result;
    }
    bool empty() const {
        const auto& e=(_editing?_draft:_document->asset()).emitters[_emitter].effect;
        return _kind==2?e.colorGradient.empty():scalarKeys(e).empty();
    }
    std::shared_ptr<const ui::authoring::CurveTrack> curveTrack(const std::string&) const override {
        const auto current=revision(); if(_cache && _cacheRevision==current) return _cache;
        auto result=std::make_shared<ui::authoring::CurveTrack>(); result->id="lifetime";
        const auto e=(_editing?_draft:_document->asset()).emitters[_emitter].effect;
        if(_kind==2) {
            auto keys=e.colorGradient;
            if(keys.empty()) keys={{0,e.startColor},{1,e.endColor}};
            for(size_t i=0;i<keys.size();++i) { const auto& c=keys[i].color;
                result->keys.push_back({std::to_string(i),keys[i].time,{c.r,c.g,c.b,c.a},{},{}}); }
            result->sample=[keys](size_t c,double t) {
                const auto v=particle::sampleGradient(keys,float(t),{});
                return c==0?v.r:c==1?v.g:c==2?v.b:v.a;
            };
        } else {
            auto keys=scalarKeys(e);
            if(keys.empty()) keys={{0,_kind==0?1:e.startColor.a},{1,_kind==0?e.endSizeScale:e.endColor.a}};
            for(size_t i=0;i<keys.size();++i) result->keys.push_back({std::to_string(i),keys[i].time,{keys[i].value},{},{}});
            result->sample=[keys](size_t,double t) { return particle::sampleCurve(keys,float(t),1); };
        }
        _cache=result; _cacheRevision=current; return result;
    }
private:
    template<class Effect> auto& scalarKeys(Effect& e) const {
        if(_kind<3)return _kind==0?e.sizeCurve:e.opacityCurve;
        auto& p=e.value(static_cast<particle::FloatAttribute>((_kind-3)/2));
        return (_kind-3)%2?p.upperCurve:p.curve;
    }
    float scalarValue(float v) const {return _kind==0?std::max(0.f,v):_kind==1?std::clamp(v,0.f,1.f):v;}
    std::shared_ptr<EditorParticleDocument> _document;
    size_t _emitter;
    int _kind;
    bool _editing=false;
    particle::EffectAsset _draft;
    uint64_t _serial=0,_baseRevision=0;
    mutable uint64_t _cacheRevision=std::numeric_limits<uint64_t>::max();
    mutable std::shared_ptr<const ui::authoring::CurveTrack> _cache;
};
}
