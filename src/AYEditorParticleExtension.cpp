#include <AYEditor/EditorParticleDocument.h>
#include <AYEditor/EditorCommandButtons.h>
#include <AYEditor/EditorProjectDescriptor.h>
#include "AYEditorParticleCurveSource.h"
#include "AYEditorParticlePreview.h"
#include <AYUI/Authoring/CurveCanvas.h>
#include <AYParticle/EffectResource.h>
#include <AYResource/AssetPath.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ColorPicker.h>
#include <AYUI/ScrollView.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif
namespace ayt::editor {
namespace {
using namespace ayt::ui;
TextLabel* label(const std::wstring& text) {
    auto* w=new TextLabel(); w->setText(text); w->setFontSize(12); return w;
}
std::wstring number(float value) {
    wchar_t text[48]; std::swprintf(text,48,L"%.6g",static_cast<double>(value)); return text;
}
class ParticleCanvas final : public Widget {
public:
    ParticleCanvas(std::shared_ptr<EditorParticleDocument> document,IEditorHostServices& host,
                   std::function<size_t()> selectedLayer,std::function<void(size_t)> selectLayer)
         :_document(std::move(document)),_host(host),_runtime(_document),
          _selectedLayer(std::move(selectedLayer)),_selectLayer(std::move(selectLayer)) {
        setId("particle_preview");
        _scene=_document->previewSettings().scene3D;
        _yaw=_document->previewSettings().yaw;_pitch=_document->previewSettings().pitch;
    }
    void shutdown() {_runtime.shutdown();}
    void focus() {_runtime.focus();markDirty();}
    void setSceneView(bool scene) {_scene=scene;_document->previewSettings().scene3D=scene;markDirty();}
    bool onMouseButtonDown(const UIMouseEvent& e) override {
        if(e.mouseButton==1&&_scene){_orbit=true;_last=e.mousePos;return true;}
        if(e.mouseButton!=0||_scene)return false;
        const auto b=getWorldBounds();const auto& layers=_document->asset().emitters;
        const float scale=(b.maxY-b.minY)/_document->previewSettings().screenHeight;
        if(scale<=0)return false;
        const auto hit=[&](size_t n) {
            if(!_document->previewLayerVisible(n)||layers[n].effect.dimension!=particle::Dimension::TwoD)return false;
            const auto origin=screenPoint(layers[n].offset,b,scale);
            const auto& effect=layers[n].effect;
            const float extentX=std::max(effect.extent.x*scale,22.f);
            const float extentY=std::max(effect.extent.y*scale,22.f);
            const bool sizeX=(effect.shape==particle::Shape::Rectangle||effect.shape==particle::Shape::Disk)
                && std::abs(e.mousePos.x-(origin.x+extentX))<=10&&std::abs(e.mousePos.y-origin.y)<=10;
            const bool sizeY=effect.shape==particle::Shape::Rectangle
                && std::abs(e.mousePos.x-origin.x)<=10&&std::abs(e.mousePos.y-(origin.y-extentY))<=10;
            const bool moveHandle=std::abs(e.mousePos.x-origin.x)<=12&&std::abs(e.mousePos.y-origin.y)<=12;
            if(!sizeX&&!sizeY&&!moveHandle)return false;
            _drag=n;_resizeAxis=sizeX?1:sizeY?2:0;_dragStart=e.mousePos;_draft=layers[n];_dragRevision=_document->revision();
            _selectLayer(n);markDirty();return true;
        };
        const size_t selected=_selectedLayer();
        if(selected<layers.size()&&hit(selected))return true;
        for(size_t n=layers.size();n-->0;)if(n!=selected&&hit(n))return true;
        return false;
    }
    bool onMouseButtonUp(const UIMouseEvent& e) override {
        if(e.mouseButton==1&&_orbit){_orbit=false;return true;}
        if(e.mouseButton!=0||!_drag)return false;
        updateDraft(e.mousePos);if(!_drag)return true;
        const size_t n=*_drag;_drag.reset();markDirty();
        if(_dragRevision!=_document->revision()||_draft==_document->asset().emitters[n])return true;
        auto next=_document->asset();next.emitters[n]=_draft;
        std::string error;if(!_document->replaceAsset(next,&error))_host.setStatusText(decodeUtf8Text(error));return true;
    }
    bool onMouseMove(const UIMouseEvent& e) override {
        if(_drag){updateDraft(e.mousePos);markDirty();return true;}
        if(!_orbit)return false;_yaw+=(e.mousePos.x-_last.x)*.008f;_pitch=std::clamp(_pitch+(e.mousePos.y-_last.y)*.008f,-1.4f,1.4f);_last=e.mousePos;auto& s=_document->previewSettings();s.yaw=_yaw;s.pitch=_pitch;markDirty();return true;
    }
    void onCaptureCancelled() override {_orbit=false;_drag.reset();markDirty();}
    void invalidateTextures() { _textures.clear(); _attempted.clear(); markDirty(); }
    bool onMouseWheel(const UIMouseWheelEvent& e) override {
        _scale=std::clamp(_scale*(e.deltaY>0?1.15f:0.87f),8.0f,512.0f);
        auto& s=_document->previewSettings();s.distance=std::clamp(s.distance*(e.deltaY>0?.87f:1.15f),.2f,3000.f);
        s.screenHeight=std::clamp(s.screenHeight*(e.deltaY>0?.87f:1.15f),.1f,2000.f);markDirty();return true;
    }
protected:
    void onRender(IRenderBackend& renderer) override {
        const auto b=getWorldBounds();
        if(auto* gpu=dynamic_cast<render::UIRenderBackend*>(&renderer)) {
            if(_runtime.render(*gpu,b)) {
                renderer.pushClip(b);
                renderer.drawText({b.minX+8,b.minY+4,b.maxX-8,b.minY+27},decodeUtf8Text(_runtime.status()),11,math::FVector4{.72f,.8f,.9f,1});
                drawHandles(renderer,b);renderer.popClip();return;
            }
        }
        _document->setRuntimeGpuActive(false);
        renderer.drawRect(b,{0.035f,0.045f,0.065f,1});
        const float x=(b.minX+b.maxX)/2,y=(b.minY+b.maxY)/2;
        renderer.pushClip(b);
        renderer.drawRect({b.minX,y,b.maxX,y+1},{0.16f,0.18f,0.22f,1});
        renderer.drawRect({x,b.minY,x+1,b.maxY},{0.16f,0.18f,0.22f,1});
        const auto& settings=_document->previewSettings();
        const float drawScale=_scene?_scale:(b.maxY-b.minY)/settings.screenHeight;
        const particle::Vec3 right=_scene?particle::Vec3{std::cos(_yaw),0,-std::sin(_yaw)}:particle::Vec3{1,0,0};
        const particle::Vec3 up=_scene?particle::Vec3{std::sin(_yaw)*std::sin(_pitch),std::cos(_pitch),std::cos(_yaw)*std::sin(_pitch)}:particle::Vec3{0,1,0};
        const auto dot=[](particle::Vec3 a,particle::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
        if(_scene)for(int n=-5;n<=5;++n) {
            const float gy=y-float(n)*dot({0,0,1},up)*_scale;
            renderer.drawRect({b.minX,gy,b.maxX,gy+1},{.08f,.11f,.15f,1});
        }
        const auto* preview=_document->preview();
        if(preview) for(size_t i=0;i<preview->instances().size();++i) {
            if(!_document->previewLayerVisible(i))continue;
            const auto& instance=preview->instances()[i]; const auto& e=instance.effect();
            void* texture=nullptr;
            if(!e.texturePath.empty()) {
                auto& image=_textures[e.texturePath];
                if(!_attempted.contains(e.texturePath)) {
                    _attempted.insert(e.texturePath);
                    image=_host.loadAuthoringImage(resource::resolveAssetPath(_document->path(),e.texturePath));
                }
                texture=image.texture.handle;
            }
            renderer.flushBatches();
            renderer.setBlendMode(e.blend==particle::Blend::Additive?BlendMode::Additive:BlendMode::Normal);
            for(const auto& p:instance.particles()) {
                const auto s=instance.sample(p,preview->emitterPose(i));
                if(s.size<=0 || s.color.a<=0) continue;
                const float px=x+(dot(s.position,right)-(_scene?0.f:settings.focus.x))*drawScale;
                const float py=y-(dot(s.position,up)-(_scene?0.f:settings.focus.y))*drawScale;
                const float r=s.size*drawScale/2;
                if(!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(r) || !std::isfinite(s.rotation)
                    || !std::isfinite(s.color.r) || !std::isfinite(s.color.g) || !std::isfinite(s.color.b) || !std::isfinite(s.color.a)) continue;
                const math::FVector4 color{s.color.r,s.color.g,s.color.b,s.color.a};
                auto transform=math::Float4x4::identity();
                const float c=std::cos(s.rotation),sn=std::sin(s.rotation);
                transform(0,0)=c; transform(0,1)=sn; transform(1,0)=-sn; transform(1,1)=c;
                if(s.oriented) {transform(0,0)=dot(s.right,right);transform(1,0)=-dot(s.right,up);transform(0,1)=-dot(s.up,right);transform(1,1)=dot(s.up,up);}
                if(s.flipX){transform(0,0)=-transform(0,0);transform(1,0)=-transform(1,0);}
                if(s.flipY){transform(0,1)=-transform(0,1);transform(1,1)=-transform(1,1);}
                transform(0,3)=px; transform(1,3)=py;
                renderer.pushTransform(transform);
                if(texture) {
                    const float u=float(s.frame%e.columns)/e.columns,v=float(s.frame/e.columns)/e.rows;
                    renderer.addTexturedQuad({-r,-r,r,r},texture,{u,v,u+1.0f/e.columns,v+1.0f/e.rows},color);
                } else renderer.drawRoundedRect({-r,-r,r,r},color,r);
                renderer.flushBatches(); renderer.popTransform();
            }
        }
        renderer.setBlendMode(BlendMode::Normal);
        renderer.drawText({b.minX+8,b.minY+4,b.maxX-8,b.minY+25},
            _scene?L"Software fallback · 3D orthographic · runtime preview unavailable":L"Software fallback · 2D screen · runtime preview unavailable",11,math::FVector4{0.6f,0.65f,0.75f,1});
        drawHandles(renderer,b);
        renderer.popClip();
    }
private:
    math::FVector2 screenPoint(particle::Vec3 offset,const math::FRectangle& b,float scale) const {
        const auto focus=_document->previewSettings().focus;
        return {(b.minX+b.maxX)*.5f+(offset.x-focus.x)*scale,
                (b.minY+b.maxY)*.5f-(offset.y-focus.y)*scale};
    }
    void updateDraft(math::FVector2 point) {
        if(_dragRevision!=_document->revision()||*_drag>=_document->asset().emitters.size()) {_drag.reset();return;}
        const auto b=getWorldBounds();const float scale=(b.maxY-b.minY)/_document->previewSettings().screenHeight;
        if(scale<=0)return;
        const float dx=(point.x-_dragStart.x)/scale,dy=(_dragStart.y-point.y)/scale;
        const auto& original=_document->asset().emitters[*_drag];
        if(_resizeAxis==1) {
            _draft.effect.extent.x=std::max(0.f,original.effect.extent.x+dx);
            if(_draft.effect.shape==particle::Shape::Disk)_draft.effect.extent.y=_draft.effect.extent.x;
        } else if(_resizeAxis==2)_draft.effect.extent.y=std::max(0.f,original.effect.extent.y+dy);
        else {_draft.offset.x=original.offset.x+dx;_draft.offset.y=original.offset.y+dy;}
    }
    void drawHandles(IRenderBackend& r,const math::FRectangle& b) {
        if(_scene)return;
        const float scale=(b.maxY-b.minY)/_document->previewSettings().screenHeight;
        if(scale<=0)return;
        const auto& layers=_document->asset().emitters;
        for(size_t n=0;n<layers.size();++n) {
            if(!_document->previewLayerVisible(n)||layers[n].effect.dimension!=particle::Dimension::TwoD)continue;
            const auto& layer=(_drag&&*_drag==n)?_draft:layers[n];const auto p=screenPoint(layer.offset,b,scale);
            const math::FVector4 color=(_drag&&*_drag==n)?math::FVector4{1,.75f,.27f,1}:math::FVector4{.34f,.76f,1,1};
            r.drawRect({p.x-9,p.y-1,p.x+9,p.y+1},color);r.drawRect({p.x-1,p.y-9,p.x+1,p.y+9},color);
            if(layer.effect.shape==particle::Shape::Rectangle||layer.effect.shape==particle::Shape::Disk) {
                const float x=p.x+std::max(layer.effect.extent.x*scale,22.f);
                r.drawRect({x-4,p.y-4,x+4,p.y+4},color);
            }
            if(layer.effect.shape==particle::Shape::Rectangle) {
                const float y=p.y-std::max(layer.effect.extent.y*scale,22.f);
                r.drawRect({p.x-4,y-4,p.x+4,y+4},color);
            }
        }
    }
    std::shared_ptr<EditorParticleDocument> _document;
    IEditorHostServices& _host;
    EditorParticleRuntimePreview _runtime;
    std::function<size_t()> _selectedLayer;
    std::function<void(size_t)> _selectLayer;
    float _scale=90,_yaw=.5f,_pitch=.3f;
    bool _scene=false,_orbit=false;math::FVector2 _last{};
    std::optional<size_t> _drag;int _resizeAxis=0;math::FVector2 _dragStart{};
    uint64_t _dragRevision=0;particle::EffectEmitter _draft;
    std::unordered_map<std::string,EditorAuthoringImage> _textures;
    std::unordered_set<std::string> _attempted;
};
class ParticleActivity final : public Widget {
public:
    explicit ParticleActivity(std::shared_ptr<EditorParticleDocument> d):_document(std::move(d)){setId("particle_activity");}
    math::FVector2 getPreferredContentSize() const override {return {400,26.f+21.f*float(_document->asset().emitters.size())};}
    bool onMouseButtonDown(const UIMouseEvent& e) override {
        if(e.mouseButton!=0)return false;const auto b=getWorldBounds();
        if(b.maxX-b.minX<=120)return false;
        const float t=std::clamp((e.mousePos.x-b.minX-120)/(b.maxX-b.minX-120),0.f,1.f);
        return _document->seek(t*std::min(60.f,_document->asset().duration));
    }
protected:
    void onRender(IRenderBackend& r) override {
        const auto b=getWorldBounds();r.drawRect(b,{.04f,.05f,.075f,1});r.pushClip(b);
        auto a=_document->asset();a.seed=_document->previewSeed();
        const float duration=std::min(60.f,a.duration),start=b.minX+120,width=std::max(1.f,b.maxX-start);
        r.drawText({b.minX+6,b.minY,b.maxX,b.minY+22},L"Layers / click to seek · 0—"+number(duration)+L" s",11,math::FVector4{.65f,.7f,.8f,1});
        const auto cycle=uint64_t(_document->previewTime()/a.duration);
        for(size_t i=0;i<a.emitters.size();++i) {
            const float y=b.minY+25+float(i)*21;if(y>b.maxY)break;
            const auto& layer=a.emitters[i];const auto effect=particle::layerEffect(layer);
            r.drawText({b.minX+6,y,start-4,y+20},decodeUtf8Text(layer.name),11,math::FVector4{.7f,.75f,.8f,1});
            const float delay=particle::layerDelay(a,i,cycle);
            const float end=effect.looping?duration:std::min(duration,delay+effect.duration+effect.lifetime.max);
            const bool enabled=particle::layerEnabled(a,i,cycle);
            const math::FVector4 color=enabled?math::FVector4{.26f,.57f,.8f,.7f}:math::FVector4{.2f,.23f,.28f,.5f};
            r.drawRoundedRect({start+width*std::min(delay/duration,1.f),y+3,start+width*end/duration,y+16},color,3);
        }
        const float phase=float(std::fmod(_document->previewTime(),double(a.duration)));
        const float cursor=start+width*std::min(phase/duration,1.f);r.drawRect({cursor,b.minY+23,cursor+1,b.maxY},{.9f,.74f,.4f,1});r.popClip();
    }
private: std::shared_ptr<EditorParticleDocument> _document;
};
class ParticleGradientStrip final : public Widget {
public:
    explicit ParticleGradientStrip(std::vector<particle::GradientKey> keys):_keys(std::move(keys)) { setId("particle_gradient_strip"); }
protected:
    void onRender(IRenderBackend& renderer) override {
        const auto b=getWorldBounds();
        renderer.pushClip(b);
        for(float y=b.minY;y<b.maxY;y+=8) for(float x=b.minX;x<b.maxX;x+=8) {
            const bool alternate=(int((x-b.minX)/8)+int((y-b.minY)/8))%2;
            const float v=alternate?0.22f:0.38f;
            renderer.drawRect({x,y,x+8,y+8},{v,v,v,1});
        }
        for(int i=0;i<128;++i) {
            const auto c=particle::sampleGradient(_keys,(float(i)+0.5f)/128,{});
            const float left=b.minX+(b.maxX-b.minX)*float(i)/128;
            const float right=b.minX+(b.maxX-b.minX)*float(i+1)/128;
            renderer.drawRect({left,b.minY,right,b.maxY},{c.r,c.g,c.b,c.a});
        }
        renderer.popClip();
    }
private:
    std::vector<particle::GradientKey> _keys;
};
class ParticleTexturePreview final : public Widget {
public:
    ParticleTexturePreview(std::shared_ptr<EditorParticleDocument> document,IEditorHostServices& host,size_t layer)
        :_document(std::move(document)),_host(host),_layer(layer) {setId("particle_texture_preview");}
protected:
    void onRender(IRenderBackend& r) override {
        const auto b=getWorldBounds();r.drawRect(b,{.09f,.11f,.15f,1});
        if(_layer>=_document->asset().emitters.size())return;
        const auto& e=_document->asset().emitters[_layer].effect;
        if(e.texturePath.empty()) {r.drawText(b,L"Default soft particle · no texture",11,math::FVector4{.7f,.75f,.82f,1});return;}
        if(!_attempted) {
            _attempted=true;_image=_host.loadAuthoringImage(resource::resolveAssetPath(_document->path(),e.texturePath));
        }
        if(!_image) {r.drawText(b,L"Texture unavailable · check the path",11,math::FVector4{1,.52f,.45f,1});return;}
        const float mid=(b.minX+b.maxX)*.5f;
        const math::FRectangle white{0,0,1,1};
        r.addTexturedQuad({b.minX+4,b.minY+4,mid-4,b.maxY-4},_image.texture.handle,white);
        const uint32_t columns=std::max(1u,e.columns),rows=std::max(1u,e.rows);
        const uint64_t total=uint64_t(columns)*rows;
        const uint32_t first=std::min<uint64_t>(e.firstFrame,total-1);
        const uint32_t count=std::max<uint32_t>(1u,static_cast<uint32_t>(std::min<uint64_t>(e.frameCount,total-first)));
        const auto steps=uint64_t(std::clamp(std::floor(_document->previewTime()*std::max(0.f,e.framesPerSecond)),0.0,1.0e12));
        uint32_t frame=uint32_t(steps%count);
        if(e.animationPlayback==particle::AnimationPlayback::Once)frame=uint32_t(std::min<uint64_t>(steps,count-1));
        if(e.animationPlayback==particle::AnimationPlayback::PingPong&&count>1) {
            const auto phase=uint32_t(steps%(2*count-2));frame=phase<count?phase:2*count-2-phase;
        }
        if(e.reverseAnimation.mode==particle::BoolMode::Fixed&&e.reverseAnimation.value)frame=count-1-frame;
        frame+=first;
        const float u=float(frame%columns)/columns,v=float(frame/columns)/rows;
        r.addTexturedQuad({mid+4,b.minY+4,b.maxX-4,b.maxY-4},_image.texture.handle,
                          {u,v,u+1.f/columns,v+1.f/rows});
        r.drawText({mid+6,b.maxY-22,b.maxX-4,b.maxY},L"Frame "+std::to_wstring(frame+1),11,math::FVector4{1,1,1,1});
    }
private:
    std::shared_ptr<EditorParticleDocument> _document;
    IEditorHostServices& _host;size_t _layer=0;bool _attempted=false;EditorAuthoringImage _image;
};
class ParticleTextureTile final : public Widget {
public:
    ParticleTextureTile(std::string reference,std::string absolute,IEditorHostServices& host,
                        std::function<void(const std::string&)> choose)
        :_reference(std::move(reference)),_absolute(std::move(absolute)),_host(host),_choose(std::move(choose)) {
        setId("particle_texture_tile_"+_reference);
    }
    bool onMouseButtonDown(const UIMouseEvent& e) override {return e.mouseButton==0;}
    bool onMouseButtonUp(const UIMouseEvent& e) override {
        if(e.mouseButton!=0)return false;
        const auto b=getWorldBounds();
        if(e.mousePos.x<b.minX||e.mousePos.x>b.maxX||e.mousePos.y<b.minY||e.mousePos.y>b.maxY)return false;
        _choose(_reference);return true;
    }
protected:
    void onRender(IRenderBackend& r) override {
        const auto b=getWorldBounds();r.drawRect(b,{.13f,.17f,.22f,1});
        if(!_attempted){_attempted=true;_image=_host.loadAuthoringImage(_absolute);}
        r.pushClip(b);
        if(_image)r.addTexturedQuad({b.minX+3,b.minY+3,b.maxX-3,b.maxY-22},
                                    _image.texture.handle,{0,0,1,1});
        else r.drawText({b.minX+4,b.minY+4,b.maxX-4,b.maxY-22},L"No thumbnail",11,math::FVector4{.7f,.7f,.75f,1});
        r.drawText({b.minX+4,b.maxY-21,b.maxX-4,b.maxY},
                   decodeUtf8Text(std::filesystem::path(_reference).filename().string()),11,
                   math::FVector4{.86f,.9f,.95f,1});
        r.popClip();
    }
private:
    std::string _reference,_absolute;IEditorHostServices& _host;
    std::function<void(const std::string&)> _choose;
    bool _attempted=false;EditorAuthoringImage _image;
};
class ParticleView final : public IEditorView {
public:
    ParticleView(std::shared_ptr<EditorParticleDocument> document,IEditorHostServices& host)
        :_document(std::move(document)),_host(host),
         _commands([this] { return _document.get(); }) {
        auto* root=new VBox(); _root=root; root->setId("particle_workspace");
        root->setSpacing(6); root->setPadding(8,8,8,8);
        auto* transport=new HBox(); transport->setSpacing(4);
        _commands.add(*transport,L"Play","particle.play",60);
        _commands.add(*transport,L"Pause","particle.pause",60);
        _commands.add(*transport,L"Restart","particle.restart",70);
        _commands.add(*transport,L"Stop","particle.stop",60);
        _commands.add(*transport,L"Step","particle.step",60);
        _commands.add(*transport,L"Undo","edit.undo",60);
        _commands.add(*transport,L"Redo","edit.redo",60);
        root->addWidget(transport,30);
        auto* files=new HBox(); files->setSpacing(4);
        _commands.add(*files,L"Save","file.save",65);
        _commands.add(*files,L"Save As","file.save_as",75);
        _commands.add(*files,L"Reload","file.reload",65);
        root->addWidget(files,30);
        _document->setSavePathProvider([this](bool saveAs) {
            if(auto* provider=dynamic_cast<IEditorDocumentSavePathProvider*>(&_host))
                return provider->chooseDocumentSavePath(*_document,saveAs);
#ifdef _WIN32
            wchar_t path[32768]={};
            const auto current=decodeUtf8Text(_document->path());
            if(current.size()<32768) std::copy(current.begin(),current.end(),path);
            const auto folder=(std::filesystem::path(_host.projectRoot())/"Assets/effects").wstring();
            OPENFILENAMEW dialog{}; dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=GetActiveWindow();
            dialog.lpstrFile=path; dialog.nMaxFile=32768; dialog.lpstrInitialDir=folder.c_str();
            dialog.lpstrFilter=L"Particle Effect (*.pfx)\0*.pfx\0Legacy Particle Effect (*.ayparticle)\0*.ayparticle\0";
            dialog.lpstrDefExt=L"pfx";
            dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            return GetSaveFileNameW(&dialog)?encodeUtf8Text(path):std::string{};
#else
            return std::string{};
#endif
        });
        _stats=label(L""); _stats->setId("particle_stats"); root->addWidget(_stats,24);
        auto* body=new HBox(); body->setSpacing(10); root->addWidget(body,0);
        auto* layerScroll=new ScrollView();layerScroll->setId("particle_layers_scroll");
        _layers=new VBox();_layers->setSpacing(5);layerScroll->addChild(_layers);body->addWidget(layerScroll,180);
        auto* center=new VBox();center->setSpacing(6);body->addWidget(center,0);
        auto* views=new HBox();views->setSpacing(4);center->addWidget(views,28);
        _canvas=new ParticleCanvas(_document,host,[this]{return _selected;},
            [this](size_t n){_selected=n;_revision=0;});center->addWidget(_canvas,0);
        button(views,L"2D screen",[this]{_canvas->setSceneView(false);},90);views->getChildren().back()->setId("particle_view_2d");
        button(views,L"3D scene",[this]{_canvas->setSceneView(true);},90);views->getChildren().back()->setId("particle_view_3d");
        button(views,L"New variation",[this]{_document->setPreviewSeed(_document->previewSeed()+1);},110);views->getChildren().back()->setId("particle_preview_reroll");
        button(views,L"Asset seed",[this]{_document->useAssetSeed();},90);
        auto* previewTools=new HBox();previewTools->setSpacing(4);center->addWidget(previewTools,28);
        auto* backend=new ComboBox();backend->setId("particle_preview_backend");backend->setItems({L"CPU",L"GPU",L"Auto"});
        backend->setSelectedIndex(int(_document->previewSettings().backend));
        backend->setOnSelectionChanged([this,alive=_alive](int n){if(*alive)_document->previewSettings().backend=static_cast<particle::Backend>(n);});
        previewTools->addWidget(backend,85);
        button(previewTools,L"Focus",[this]{_canvas->focus();},60);previewTools->getChildren().back()->setId("particle_preview_focus");
        auto* background=new ComboBox();background->setId("particle_preview_background");background->setItems({L"Dark",L"Light",L"Black"});
        background->setSelectedIndex(_document->previewSettings().background==0xd0d7e2ff?1:_document->previewSettings().background==0x000000ff?2:0);
        background->setOnSelectionChanged([this,alive=_alive](int n){if(*alive)_document->previewSettings().background=n==1?0xd0d7e2ff:n==2?0x000000ff:0x101722ff;});
        previewTools->addWidget(background,80);
        auto* ground=new CheckBox();ground->setId("particle_preview_ground");ground->setText(L"Ground");ground->setChecked(_document->previewSettings().ground);
        ground->setOnToggled([this,alive=_alive](bool v){if(*alive)_document->previewSettings().ground=v;});previewTools->addWidget(ground,80);
        auto* object=new CheckBox();object->setId("particle_preview_object");object->setText(L"Object");object->setChecked(_document->previewSettings().testObject);
        object->setOnToggled([this,alive=_alive](bool v){if(*alive)_document->previewSettings().testObject=v;});previewTools->addWidget(object,80);
        _activity=new ParticleActivity(_document);auto* activityScroll=new ScrollView();activityScroll->setId("particle_activity_scroll");activityScroll->addChild(_activity);center->addWidget(activityScroll,130);
        auto* scroll=new ScrollView(); scroll->setId("particle_properties_scroll");
        _properties=new VBox(); _properties->setSpacing(5); _properties->setPadding(5,5,5,5);
        scroll->addChild(_properties); body->addWidget(scroll,330);
        _message=label(L""); _message->setId("particle_message"); root->addWidget(_message,24);
        rebuild(); tick(0);
    }
    ~ParticleView() override {
        prepareForUiShutdown();
        if(_ownsRoot && _root) destroyWidgetTree(_root);
    }
    Widget* rootWidget() noexcept override { return _root; }
    Widget* releaseRootWidget() noexcept override { _ownsRoot=false; return _root; }
    IEditorCommandTarget* commandTarget() noexcept override { return _document.get(); }
    void onDeactivated() override { if(_curveCanvas) _curveCanvas->onCaptureCancelled();if(_canvas)_canvas->onCaptureCancelled(); _document->pause(); }
    void prepareForUiShutdown() override { if(_canvas) {_canvas->onCaptureCancelled();_canvas->shutdown();} _canvas=nullptr; if(_curveCanvas) _curveCanvas->onCaptureCancelled(); _curveCanvas=nullptr; *_alive=false; _commands.detach(); _document->setSavePathProvider({}); }
    void tick(float dt) override {
        if(!*_alive) return;
        _document->tick(dt);
        if(_revision!=_document->revision() && (!_curveSource || !_curveSource->editing())) rebuild();
        _commands.refresh();
        if(!_document->diagnostic().empty()) _message->setText(decodeUtf8Text(_document->diagnostic()));
        const auto* p=_document->preview();
        wchar_t text[160]; std::swprintf(text,160,L"%ls  ·  %.2f s  ·  %zu particles  ·  Seed %u  ·  %.3f ms",
            _document->playing()?L"Playing":L"Paused",_document->previewTime(),p?p->liveParticles():0,
            _document->previewSeed(),_document->simulationMilliseconds());
        if(_document->runtimeGpuActive()) {
            uint64_t capacity=0;for(const auto& layer:_document->asset().emitters)capacity+=particle::layerEffect(layer).capacity;
            std::swprintf(text,160,L"%ls  ·  %.2f s  ·  GPU capacity %llu  ·  Seed %u",
                _document->playing()?L"Playing":L"Paused",_document->previewTime(),static_cast<unsigned long long>(capacity),_document->previewSeed());
        }
        _stats->setText(text); _canvas->markDirty();_activity->markDirty();
    }
private:
    using Change=std::function<void(particle::EffectAsset&)>;
    void apply(Change change) {
        auto next=_document->asset();
        try { change(next); } catch(const std::exception& e) { _message->setText(decodeUtf8Text(e.what())); return; }
        std::string error;
        if(!_document->replaceAsset(next,&error)) { _selected=std::min(_selected,_document->asset().emitters.size()-1); _message->setText(decodeUtf8Text(error)); }
        else { _message->setText(L""); _host.requestRepaint(); }
    }
    void button(HBox* row,const std::wstring& title,std::function<void()> action,float width) {
        auto* b=new Button(); b->setText(title); const auto alive=_alive;
        b->setOnClicked([alive,action=std::move(action)] { if(*alive) action(); }); row->addWidget(b,width);
    }
    void text(const std::wstring& title,const std::string& value,
              std::function<void(particle::EffectAsset&,const std::string&)> change,const std::string& id={}) {
        auto* row=new HBox(); row->setSpacing(4); row->addWidget(label(title),110);
        auto* input=new TextInput(); input->setText(decodeUtf8Text(value)); if(!id.empty()) input->setId(id);
        const auto alive=_alive;
        const auto submitted=std::make_shared<std::wstring>(input->getText());
        const auto submit=[this,alive,change,submitted](const std::wstring& v) {
            // Display rounding must not author a change merely on focus loss.
            if(!*alive || v==*submitted) return;
            const auto before=_document->revision();
            apply([&](auto& a) { change(a,encodeUtf8Text(v)); });
            if(_document->revision()!=before) *submitted=v;
        };
        input->setOnSubmit(submit);
        input->setOnFocusLostNotify([input,submit] { submit(input->getText()); });
        row->addWidget(input,0); _properties->addWidget(row,28);
    }
    void numeric(const std::wstring& title,float value,
                 std::function<void(particle::EffectAsset&,float)> change,const std::string& id={}) {
        text(title,encodeUtf8Text(number(value)),[this,change](auto& a,const std::string& s) {
            size_t parsed=0;
            try { const float v=std::stof(s,&parsed); if(parsed!=s.size() || !std::isfinite(v)) throw std::invalid_argument("number");
                change(a,v);
            } catch(...) { throw std::invalid_argument("Enter a finite number"); }
        },id);
    }
    void choice(const std::wstring& title,const std::vector<std::wstring>& items,int selected,
                std::function<void(particle::EffectAsset&,int)> change,const std::string& id={}) {
        auto* row=new HBox(); row->setSpacing(4); row->addWidget(label(title),110);
        auto* input=new ComboBox(); input->setItems(items); input->setSelectedIndex(selected);
        if(!id.empty()) input->setId(id);
        const auto alive=_alive; input->setOnSelectionChanged([this,alive,change](int v) {
            if(*alive) apply([&](auto& a) { change(a,v); });
        });
        row->addWidget(input,0); _properties->addWidget(row,28);
    }
    void boolean(const std::wstring& title,bool value,Change yes,Change no) {
        auto* box=new CheckBox(); box->setText(title); box->setChecked(value); const auto alive=_alive;
        box->setOnToggled([this,alive,yes,no](bool v) { if(*alive) apply(v?yes:no); });
        _properties->addWidget(box,28);
    }

    using NumberAccess=std::function<particle::NumberProperty&(particle::EffectAsset&)>;
    using BoolAccess=std::function<particle::BoolProperty&(particle::EffectAsset&)>;
    void boolSource(const std::wstring& title,const particle::BoolProperty& p,BoolAccess access,bool birth,const std::string& id) {
        choice(title,{L"Fixed",L"Probability"},int(p.mode),[access,birth](auto& a,int n){auto& p=access(a);p.mode=static_cast<particle::BoolMode>(n);if(!birth&&p.scope==particle::SampleScope::ParticleBirth)p.scope=particle::SampleScope::EffectPlay;},id+"_mode");
        if(p.mode==particle::BoolMode::Fixed)choice(L"Value",{L"False",L"True"},p.value?1:0,[access](auto& a,int n){access(a).value=n!=0;},id+"_fixed");
        else {numeric(L"Probability 0—1",p.probability,[access](auto& a,float v){access(a).probability=v;},id+"_probability");
            choice(L"Choose once",birth?std::vector<std::wstring>{L"Effect play",L"Composite cycle",L"Particle birth"}:std::vector<std::wstring>{L"Effect play",L"Composite cycle"},int(p.scope),[access](auto& a,int n){access(a).scope=static_cast<particle::SampleScope>(n);},id+"_scope");}
    }
    void numberSource(const particle::NumberProperty& p,NumberAccess access,float fallback,bool curves,bool birth,const std::string& id) {
        std::vector<std::wstring> modes{L"Existing values",L"Constant",L"Random range",L"Weighted choices"};
        if(curves){modes.push_back(L"Curve");modes.push_back(L"Random between curves");}
        choice(L"Value mode",modes,int(p.mode),[access,fallback,birth](auto& a,int n){auto& p=access(a);p.mode=static_cast<particle::NumberMode>(n);
            if(!birth&&p.scope==particle::SampleScope::ParticleBirth)p.scope=particle::SampleScope::EffectPlay;
            p.value=p.min=p.max=fallback;if(p.choices.empty())p.choices={{fallback,1}};
            if(p.curve.empty())p.curve={{0,fallback},{1,fallback}};if(p.upperCurve.empty())p.upperCurve=p.curve;
        },id+"_mode");
        if(p.mode==particle::NumberMode::Legacy)return;
        choice(L"Choose once",birth?std::vector<std::wstring>{L"Effect play",L"Composite cycle",L"Particle birth"}:std::vector<std::wstring>{L"Effect play",L"Composite cycle"},int(p.scope),[access](auto& a,int n){access(a).scope=static_cast<particle::SampleScope>(n);},id+"_scope");
        if(p.mode==particle::NumberMode::Constant)numeric(L"Value",p.value,[access](auto& a,float v){access(a).value=v;},id+"_constant");
        if(p.mode==particle::NumberMode::Uniform){numeric(L"Minimum",p.min,[access](auto& a,float v){access(a).min=v;},id+"_min");numeric(L"Maximum",p.max,[access](auto& a,float v){access(a).max=v;},id+"_max");}
        if(p.mode==particle::NumberMode::Weighted){
            for(size_t k=0;k<p.choices.size();++k){numeric(L"Choice "+std::to_wstring(k+1),p.choices[k].value,[access,k](auto& a,float v){access(a).choices[k].value=v;},id+"_choice_"+std::to_string(k));
                numeric(L"Weight",p.choices[k].weight,[access,k](auto& a,float v){access(a).choices[k].weight=v;},id+"_weight_"+std::to_string(k));}
            auto* actions=new HBox();button(actions,L"Add choice",[this,access,fallback]{apply([=](auto& a){auto& p=access(a);if(p.choices.size()>=particle::kMaxPropertyChoices)throw std::invalid_argument("Maximum 8 choices");p.choices.push_back({fallback,1});});},110);
            button(actions,L"Remove last",[this,access]{apply([=](auto& a){auto& p=access(a);if(p.choices.size()<=1)throw std::invalid_argument("Keep at least one choice");p.choices.pop_back();});},110);_properties->addWidget(actions,28);
        }
        _properties->addWidget(label(L"Random samples stay fixed for the chosen scope."),24);
    }
    void sourceCurve(size_t i,particle::FloatAttribute key,const particle::NumberProperty& p) {
        if(p.mode!=particle::NumberMode::BetweenCurves)_sourceCurve=0;
        else {auto* picker=new ComboBox();picker->setItems({L"Lower curve",L"Upper curve"});picker->setSelectedIndex(_sourceCurve);picker->setId("particle_source_curve_select");const auto alive=_alive;
            picker->setOnSelectionChanged([this,alive](int n){if(*alive){_sourceCurve=n;_sourceKey=0;_revision=0;}});_properties->addWidget(picker,28);}
        const bool upper=_sourceCurve!=0;const auto& keys=upper?p.upperCurve:p.curve;_sourceKey=std::min(_sourceKey,keys.size()-1);
        _curveSource=std::make_shared<ParticleCurveSource>(_document,i,3+int(key)*2+int(upper));
        auto* canvas=new authoring::CurveCanvas(_curveSource);_curveCanvas=canvas;canvas->setId("particle_source_canvas");canvas->setTrackId("lifetime");
        auto state=_curveSource->selectionState();state->trackId="lifetime";state->primaryKeyId=std::to_string(_sourceKey);state->keyIds={state->primaryKeyId};
        const auto alive=_alive;canvas->setOnSelectionChanged([this,alive](const std::string& id,size_t){if(*alive&&!id.empty()){try{_sourceKey=std::stoull(id);_revision=0;}catch(...){}}});_properties->addWidget(canvas,170);
        _properties->addWidget(label(key==particle::FloatAttribute::Opacity||key==particle::FloatAttribute::SizeMultiplier?L"Time: normalized particle age":L"Time: normalized emitter phase at birth"),24);
        auto* select=new ComboBox();std::vector<std::wstring> names;for(size_t k=0;k<keys.size();++k)names.push_back(L"Key "+std::to_wstring(k+1)+L" @ "+number(keys[k].time));
        select->setItems(names);select->setSelectedIndex(int(_sourceKey));select->setId("particle_source_key");select->setOnSelectionChanged([this,alive](int n){if(*alive&&n>=0){_sourceKey=n;_revision=0;}});_properties->addWidget(select,28);
        const size_t k=_sourceKey;
        if(k>0&&k+1<keys.size())numeric(L"Key time",keys[k].time,[=](auto& a,float v){auto& p=a.emitters[i].effect.value(key);(upper?p.upperCurve:p.curve)[k].time=v;},"particle_source_time");
        numeric(L"Key value",keys[k].value,[=](auto& a,float v){auto& p=a.emitters[i].effect.value(key);(upper?p.upperCurve:p.curve)[k].value=v;},"particle_source_value");
        auto* actions=new HBox();button(actions,L"Add key",[this,i,key,upper]{apply([=](auto& a){auto& p=a.emitters[i].effect.value(key);auto& keys=upper?p.upperCurve:p.curve;
            if(keys.size()>=particle::kMaxLifetimeKeys)throw std::invalid_argument("Maximum 32 keys");size_t gap=1;for(size_t n=2;n<keys.size();++n)if(keys[n].time-keys[n-1].time>keys[gap].time-keys[gap-1].time)gap=n;
            const float t=std::midpoint(keys[gap-1].time,keys[gap].time),v=particle::sampleCurve(keys,t,0);keys.insert(keys.begin()+gap,{t,v});_sourceKey=gap;});},90);actions->getChildren().back()->setId("particle_source_add_key");
        button(actions,L"Remove key",[this,i,key,upper,k]{apply([=](auto& a){auto& p=a.emitters[i].effect.value(key);auto& keys=upper?p.upperCurve:p.curve;if(k==0||k+1>=keys.size())throw std::invalid_argument("Endpoints stay at 0 and 1");keys.erase(keys.begin()+k);_sourceKey=k-1;});},110);_properties->addWidget(actions,28);
    }
    void lifetimeEditor(size_t emitter,const particle::ParticleEffect& effect) {
        _properties->addWidget(label(L"Lifetime (0 = birth, 1 = death)"),26);
        auto* tracks=new ComboBox(); tracks->setId("particle_lifetime_track");
        tracks->setItems({L"Size multiplier",L"Opacity",L"Color gradient (RGBA)"}); tracks->setSelectedIndex(_trackKind);
        const auto alive=_alive;
        tracks->setOnSelectionChanged([this,alive](int kind) { if(*alive && kind>=0) { _trackKind=kind; _key=0; _revision=0; } });
        _properties->addWidget(tracks,28);
        const int kind=_trackKind;
        _curveSource=std::make_shared<ParticleCurveSource>(_document,emitter,kind);
        const auto snapshot=_curveSource->curveTrack("lifetime");
        _key=std::min(_key,snapshot->keys.size()-1);
        auto* canvas=new authoring::CurveCanvas(_curveSource); _curveCanvas=canvas;
        canvas->setId("particle_lifetime_canvas"); canvas->setTrackId("lifetime");
        const auto source=_curveSource;
        source->selectionState()->trackId="lifetime";
        source->selectionState()->primaryKeyId=std::to_string(_key);
        source->selectionState()->keyIds={std::to_string(_key)};
        canvas->setOnSelectionChanged([this,alive](const std::string& key,size_t) {
            if(!*alive || key.empty()) return;
            try { _key=std::stoull(key); _revision=0; } catch(...) {}
        });
        _properties->addWidget(canvas,170);
        if(kind==2) {
            auto keys=effect.colorGradient; if(keys.empty()) keys={{0,effect.startColor},{1,effect.endColor}};
            _properties->addWidget(new ParticleGradientStrip(std::move(keys)),24);
        }
        auto* actions=new HBox(); actions->setSpacing(4);
        const bool empty=source->empty();
        button(actions,empty?L"Use curve":L"Use endpoints",[this,emitter,kind,empty] {
            apply([=](auto& a) {
                auto& e=a.emitters[emitter].effect;
                if(kind==2) e.colorGradient=empty?std::vector<particle::GradientKey>{{0,e.startColor},{1,e.endColor}}:std::vector<particle::GradientKey>{};
                else { auto& keys=kind==0?e.sizeCurve:e.opacityCurve;
                    keys=empty?std::vector<particle::CurveKey>{{0,kind==0?1:e.startColor.a},{1,kind==0?e.endSizeScale:e.endColor.a}}:std::vector<particle::CurveKey>{}; }
                _key=0;
            });
        },110);
        button(actions,L"Add key",[this,emitter,kind] {
            apply([=](auto& a) {
                auto& e=a.emitters[emitter].effect;
                const auto insert=[this](auto& keys,auto sample) {
                    if(keys.empty()) throw std::invalid_argument("Choose Use curve first");
                    if(keys.size()>=particle::kMaxLifetimeKeys) throw std::invalid_argument("Maximum 32 keys");
                    size_t gap=1;
                    for(size_t n=2;n<keys.size();++n) if(keys[n].time-keys[n-1].time>keys[gap].time-keys[gap-1].time) gap=n;
                    const float t=std::midpoint(keys[gap-1].time,keys[gap].time);
                    const auto key=sample(t); keys.insert(keys.begin()+gap,key); _key=gap;
                };
                if(kind==2) insert(e.colorGradient,[&](float t) { return particle::GradientKey{t,particle::sampleGradient(e.colorGradient,t,{})}; });
                else { auto& keys=kind==0?e.sizeCurve:e.opacityCurve;
                    insert(keys,[&](float t) { return particle::CurveKey{t,particle::sampleCurve(keys,t,1)}; }); }
            });
        },80);
        actions->getChildren().back()->setId("particle_add_key");
        button(actions,L"Remove",[this,emitter,kind] {
            const size_t key=_key;
            apply([=](auto& a) {
                auto& e=a.emitters[emitter].effect;
                const auto remove=[&](auto& keys) {
                    if(key==0 || key+1>=keys.size()) throw std::invalid_argument("Endpoints stay at birth and death");
                    keys.erase(keys.begin()+key); _key=key-1;
                };
                if(kind==2) remove(e.colorGradient); else remove(kind==0?e.sizeCurve:e.opacityCurve);
            });
        },75);
        actions->getChildren().back()->setId("particle_remove_key");
        actions->getChildren().front()->setId("particle_toggle_curve");
        _properties->addWidget(actions,28);
        std::vector<std::wstring> keyNames;
        for(size_t n=0;n<snapshot->keys.size();++n) keyNames.push_back(L"Key "+std::to_wstring(n+1)+L"  @ "+number(float(snapshot->keys[n].timeSeconds)));
        auto* picker=new ComboBox(); picker->setItems(keyNames); picker->setSelectedIndex(int(_key)); picker->setId("particle_key_select");
        picker->setOnSelectionChanged([this,alive](int key) { if(*alive && key>=0) { _key=size_t(key); _revision=0; } });
        _properties->addWidget(picker,28);
        if(!empty) {
            const size_t key=_key;
            if(key!=0 && key+1<snapshot->keys.size()) numeric(L"Key lifetime",float(snapshot->keys[key].timeSeconds),[=](auto& a,float v) {
                auto& e=a.emitters[emitter].effect;
                if(kind==2)e.colorGradient[key].time=v; else (kind==0?e.sizeCurve:e.opacityCurve)[key].time=v;
            },"particle_key_time");
            if(kind==2) {
                const auto color=effect.colorGradient[key].color;
                auto* keyColor=new ColorPicker();keyColor->setId("particle_gradient_key_color");
                keyColor->setColor({color.r,color.g,color.b,color.a},false);
                keyColor->setOnColorCommitted([this,alive,emitter,key](const math::FVector4& color) {
                    if(!*alive)return;
                    apply([=](auto& a){a.emitters[emitter].effect.colorGradient[key].color={color.x,color.y,color.z,color.w};});
                });
                _properties->addWidget(keyColor,190);
            }
            for(size_t component=0;kind!=2&&component<snapshot->keys[key].values.size();++component) {
                const std::wstring title=kind!=2?L"Key value":component==0?L"Key red":component==1?L"Key green":component==2?L"Key blue":L"Key alpha";
                numeric(title,snapshot->keys[key].values[component],[=](auto& a,float v) {
                    auto& e=a.emitters[emitter].effect;
                    if(kind!=2) (kind==0?e.sizeCurve:e.opacityCurve)[key].value=v;
                    else { auto& c=e.colorGradient[key].color; if(component==0)c.r=v; else if(component==1)c.g=v; else if(component==2)c.b=v; else c.a=v; }
                },"particle_key_value_"+std::to_string(component));
            }
        }
        _properties->addWidget(label(L"Linear keys; opacity overrides color alpha."),24);
    }
    void rebuild() {
        auto* ui=_host.uiManager();
        auto activeUi=UIManager::pushActive(ui);
        if(ui) ui->clearTransientStateForSubtree(_properties);
        if(ui)ui->clearTransientStateForSubtree(_layers);
        const auto layerChildren=_layers->getChildren();for(auto* child:layerChildren){_layers->removeWidget(child);destroyWidgetTree(child);}
        const auto children=_properties->getChildren();
        _curveCanvas=nullptr;
        for(auto* child:children) { _properties->removeWidget(child); destroyWidgetTree(child); }
        _revision=_document->revision(); _canvas->invalidateTextures();
        const auto& a=_document->asset(); _selected=std::min(_selected,a.emitters.size()-1);
        _layers->addWidget(label(L"Effect layers"),26);
        for(size_t n=0;n<a.emitters.size();++n){auto* row=new HBox();
            button(row,(n==_selected?L"● ":L"○ ")+decodeUtf8Text(a.emitters[n].name),[this,n]{_selected=n;_revision=0;},0);
            row->getChildren().back()->setId("particle_layer_"+std::to_string(n));_layers->addWidget(row,32);
            auto* visibility=new HBox();visibility->setSpacing(3);
            button(visibility,_document->previewLayerHidden(n)?L"Show":L"Hide",[this,n]{_document->togglePreviewLayerHidden(n);_revision=0;},70);
            visibility->getChildren().back()->setId("particle_layer_hide_"+std::to_string(n));
            button(visibility,_document->previewLayerSolo(n)?L"Unsolo":L"Solo",[this,n]{_document->togglePreviewLayerSolo(n);_revision=0;},70);
            visibility->getChildren().back()->setId("particle_layer_solo_"+std::to_string(n));_layers->addWidget(visibility,25);
            _layers->addWidget(label(a.emitters[n].kind==particle::EffectLayerKind::Particles?L"Particle emitter":L"Sprite animation"),20);
        }
        text(L"Effect seed",std::to_string(a.seed),[](auto& a,const auto& value){size_t parsed=0;const auto n=std::stoull(value,&parsed);if(value.empty()||value[0]=='-'||parsed!=value.size()||n>UINT32_MAX)throw std::invalid_argument("Enter a whole seed 0..4294967295");a.seed=uint32_t(n);},"particle_effect_seed");
        text(L"Effect name",a.name,[](auto& a,const auto& v) { a.name=v; },"particle_effect_name");
        choice(L"Scene backend",{L"CPU",L"GPU (CPU fallback)",L"Auto"},int(a.backend),[](auto& a,int v) { a.backend=static_cast<particle::Backend>(v); },"particle_scene_backend");
        numeric(L"Cycle seconds",a.duration,[](auto& a,float v) { a.duration=v; });
        boolean(L"Repeat composition",a.looping,[](auto& a) { a.looping=true; },[](auto& a) { a.looping=false; });
        std::vector<std::wstring> names; for(const auto& e:a.emitters) names.push_back(decodeUtf8Text(e.name));
        auto* picker=new ComboBox(); picker->setId("particle_emitter_select"); picker->setItems(names);
        picker->setSelectedIndex(static_cast<int>(_selected)); const auto alive=_alive;
        picker->setOnSelectionChanged([this,alive](int v) { if(*alive && v>=0) { _selected=static_cast<size_t>(v); _revision=0; } });
        _properties->addWidget(picker,28);
        auto* add=new HBox(); add->setSpacing(4);
        const auto addPreset=[this](const particle::ParticleEffect& effect,const std::string& stem) {
            apply([&](auto& a) {
                std::string name=stem; unsigned index=2;
                const auto exists=[&](const std::string& n) { return std::any_of(a.emitters.begin(),a.emitters.end(),[&](const auto& e) { return e.name==n; }); };
                while(exists(name)) name=stem+" "+std::to_string(index++);
                uint32_t id=1;for(const auto& e:a.emitters)id=std::max(id,e.id+1);
                particle::EffectEmitter layer{name,{},0,effect};layer.id=id;a.emitters.push_back(layer);_selected=a.emitters.size()-1;
            });
        };
        button(add,L"+ Smoke",[=] { addPreset(particle::smoke(),"Smoke"); },95);
        button(add,L"+ Sparks",[=] { addPreset(particle::sparks(),"Sparks"); },95);
        button(add,L"+ Blast",[=] { addPreset(particle::explosion(),"Explosion"); },95);
        _properties->addWidget(add,28);
        auto* more=new HBox();more->setSpacing(4);
        button(more,L"+ Leaves",[=]{addPreset(particle::leaves(),"Leaves");},95);more->getChildren().back()->setId("particle_add_leaves");
        button(more,L"+ Sprite",[this]{apply([this](auto& a){particle::EffectEmitter layer;layer.name="Sprite "+std::to_string(a.emitters.size()+1);layer.kind=particle::EffectLayerKind::SpriteAnimation;
            uint32_t id=1;for(const auto& e:a.emitters)id=std::max(id,e.id+1);layer.id=id;layer.effect.looping=false;layer.effect.capacity=1;layer.effect.burst=1;layer.effect.rate=0;
            a.emitters.push_back(layer);_selected=a.emitters.size()-1;});},95);more->getChildren().back()->setId("particle_add_sprite");
        _properties->addWidget(more,28);
        auto* remove=new HBox();
        button(remove,L"Remove emitter",[this] { const auto i=_selected; apply([i](auto& a) { a.emitters.erase(a.emitters.begin()+i); }); },145);
        _properties->addWidget(remove,28);
        auto* organize=new HBox();organize->setSpacing(4);
        button(organize,L"Duplicate",[this] {const auto i=_selected;apply([this,i](auto& a) {
            if(a.emitters.size()>=particle::EffectAsset::kMaxEmitters)throw std::invalid_argument("Maximum 64 layers");
            auto copy=a.emitters[i];uint32_t id=1;for(const auto& e:a.emitters)id=std::max(id,e.id+1);copy.id=id;
            const auto stem=copy.name+" copy";copy.name=stem;unsigned n=2;
            const auto exists=[&](const std::string& name){return std::any_of(a.emitters.begin(),a.emitters.end(),[&](const auto& e){return e.name==name;});};
            while(exists(copy.name))copy.name=stem+" "+std::to_string(n++);
            a.emitters.insert(a.emitters.begin()+i+1,std::move(copy));_selected=i+1;
        });},90);organize->getChildren().back()->setId("particle_layer_duplicate");
        button(organize,L"Move up",[this] {const auto i=_selected;if(i==0)return;apply([this,i](auto& a){std::swap(a.emitters[i],a.emitters[i-1]);_selected=i-1;});},85);
        organize->getChildren().back()->setId("particle_layer_up");
        button(organize,L"Move down",[this] {const auto i=_selected;if(i+1>=_document->asset().emitters.size())return;
            apply([this,i](auto& a){std::swap(a.emitters[i],a.emitters[i+1]);_selected=i+1;});},95);
        organize->getChildren().back()->setId("particle_layer_down");_properties->addWidget(organize,28);
        const size_t i=_selected; const auto& e=a.emitters[i];
        choice(L"Layer type",{L"Particles",L"Sprite animation"},int(e.kind),[i](auto& a,int v){a.emitters[i].kind=static_cast<particle::EffectLayerKind>(v);},"particle_layer_kind");
        boolSource(L"Layer enabled",e.enabled,[i](auto& a)->auto&{return a.emitters[i].enabled;},false,"particle_enabled");
        text(L"Layer name",e.name,[i](auto& a,const auto& v) { a.emitters[i].name=v; },"particle_emitter_name");
        numeric(L"Start delay",e.delay,[i](auto& a,float v) { a.emitters[i].delay=v; });
        numberSource(e.delayValue,[i](auto& a)->auto&{return a.emitters[i].delayValue;},e.delay,false,false,"particle_delay");
        for(int axis=0;axis<3;++axis) {
            const float value=axis==0?e.offset.x:axis==1?e.offset.y:e.offset.z;
            numeric(axis==0?L"Offset X":axis==1?L"Offset Y":L"Offset Z",value,[i,axis](auto& a,float v) {
                auto& p=a.emitters[i].offset; if(axis==0)p.x=v; else if(axis==1)p.y=v; else p.z=v;
            });
        }
        const auto& f=e.effect;
        if(e.kind==particle::EffectLayerKind::SpriteAnimation&&_attribute>=int(particle::FloatAttribute::EmissionRate))_attribute=0;
        auto* attributes=new ComboBox();attributes->setId("particle_value_attribute");std::vector<std::wstring> titles;
        for(size_t k=0;k<(e.kind==particle::EffectLayerKind::SpriteAnimation?size_t(particle::FloatAttribute::EmissionRate):particle::kFloatAttributeCount);++k)titles.push_back(decodeUtf8Text(particle::attributeName(static_cast<particle::FloatAttribute>(k))));
        attributes->setItems(titles);attributes->setSelectedIndex(_attribute);
        attributes->setOnSelectionChanged([this,alive](int n){if(*alive&&n>=0){_attribute=n;_sourceKey=0;_revision=0;}});_properties->addWidget(label(L"Property source"),26);_properties->addWidget(attributes,28);
        const auto key=static_cast<particle::FloatAttribute>(_attribute);const auto& source=f.value(key);
        float fallback=1;if(key==particle::FloatAttribute::Lifetime)fallback=f.lifetime.min;else if(key==particle::FloatAttribute::Size)fallback=f.size.min;
        else if(key==particle::FloatAttribute::AnimationFps)fallback=f.framesPerSecond;else if(key==particle::FloatAttribute::Opacity)fallback=f.startColor.a;
        else if(key==particle::FloatAttribute::EmissionRate)fallback=f.rate;else if(key==particle::FloatAttribute::BurstCount)fallback=float(f.burst);
        else if(key!=particle::FloatAttribute::SizeMultiplier)fallback=0;
        const bool emission=key==particle::FloatAttribute::EmissionRate||key==particle::FloatAttribute::BurstCount;
        numberSource(source,[i,key](auto& a)->auto&{return a.emitters[i].effect.value(key);},fallback,!emission,!emission,"particle_value");
        if(source.mode==particle::NumberMode::Curve||source.mode==particle::NumberMode::BetweenCurves)sourceCurve(i,key,source);
        else lifetimeEditor(i,f);
        boolSource(L"Mirror X",f.flipX,[i](auto& a)->auto&{return a.emitters[i].effect.flipX;},true,"particle_flip_x");
        boolSource(L"Mirror Y",f.flipY,[i](auto& a)->auto&{return a.emitters[i].effect.flipY;},true,"particle_flip_y");
        boolSource(L"Reverse frames",f.reverseAnimation,[i](auto& a)->auto&{return a.emitters[i].effect.reverseAnimation;},true,"particle_reverse");
        choice(L"3D facing",{L"Camera billboard",L"Oriented plane (CPU)"},int(f.facing),[i](auto& a,int n){auto& e=a.emitters[i].effect;e.facing=static_cast<particle::ParticleFacing>(n);if(n)e.dimension=particle::Dimension::ThreeD;},"particle_facing");
        choice(L"Dimension",{L"2D",L"3D"},int(f.dimension),[i](auto& a,int v) { a.emitters[i].effect.dimension=static_cast<particle::Dimension>(v); });
        if(e.kind==particle::EffectLayerKind::Particles)choice(L"Shape",{L"Point",L"Rectangle",L"Disk",L"Box",L"Sphere"},int(f.shape),[i](auto& a,int v) { a.emitters[i].effect.shape=static_cast<particle::Shape>(v); });
        choice(L"Space",{L"Local",L"World"},int(f.space),[i](auto& a,int v) { a.emitters[i].effect.space=static_cast<particle::Space>(v); });
        choice(L"Blend",{L"Alpha",L"Additive"},int(f.blend),[i](auto& a,int v) { a.emitters[i].effect.blend=static_cast<particle::Blend>(v); });
        const auto integerField=[&](const std::wstring& title,uint32_t value,
                std::function<void(particle::ParticleEffect&,uint32_t)> change) {
            text(title,std::to_string(value),[i,change](auto& a,const std::string& text) {
                size_t parsed=0; const auto n=std::stoull(text,&parsed);
                if(text.empty() || text[0]=='-' || parsed!=text.size() || n>UINT32_MAX)
                    throw std::invalid_argument("Enter a whole unsigned count");
                change(a.emitters[i].effect,static_cast<uint32_t>(n));
            });
        };
        _properties->addWidget(label(L"Texture and flipbook"),26);
        text(L"Texture",f.texturePath,[i](auto& a,const auto& v) { a.emitters[i].effect.texturePath=v; });
        auto* textureTools=new HBox();textureTools->setSpacing(4);
        button(textureTools,L"Browse texture…",[this,i] {
#ifdef _WIN32
            wchar_t path[32768]={};
            const auto project=EditorProjectDescriptor::load(_host.projectRoot());
            const auto root=std::filesystem::path(_host.projectRoot())/(project?project.assetRoot:"Assets");
            const auto folder=(root/"textures").wstring();OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);
            dialog.hwndOwner=GetActiveWindow();dialog.lpstrFile=path;dialog.nMaxFile=32768;
            dialog.lpstrInitialDir=folder.c_str();dialog.lpstrFilter=L"Textures (*.png;*.txr;*.aytex)\0*.png;*.txr;*.aytex\0All files (*.*)\0*.*\0";
            dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
            if(!GetOpenFileNameW(&dialog))return;
            const auto selected=std::filesystem::weakly_canonical(std::filesystem::path(path));
            const auto relative=selected.lexically_relative(std::filesystem::weakly_canonical(root));
            if(relative.empty()||relative.begin()->generic_string()!="textures") {
                _message->setText(L"Choose a texture inside the project asset root's textures folder.");return;
            }
            apply([i,ref=relative.generic_string()](auto& a){a.emitters[i].effect.texturePath=ref;});
#endif
        },145);textureTools->getChildren().back()->setId("particle_texture_browse");
        button(textureTools,L"Clear",[this,i]{apply([i](auto& a){a.emitters[i].effect.texturePath.clear();});},65);
        button(textureTools,_showTextureLibrary?L"Hide tiles":L"Textures",[this]{_showTextureLibrary=!_showTextureLibrary;_revision=0;},85);
        textureTools->getChildren().back()->setId("particle_texture_gallery_toggle");
        _properties->addWidget(textureTools,28);
        _properties->addWidget(new ParticleTexturePreview(_document,_host,i),112);
        if(_showTextureLibrary) {
            const auto project=EditorProjectDescriptor::load(_host.projectRoot());
            const auto root=std::filesystem::path(_host.projectRoot())/(project?project.assetRoot:"Assets");
            const auto textureRoot=root/"textures";
            std::vector<std::pair<std::string,std::string>> candidates;
            std::error_code ec;
            for(std::filesystem::recursive_directory_iterator it(textureRoot,std::filesystem::directory_options::skip_permission_denied,ec),end;
                !ec&&it!=end&&candidates.size()<128;it.increment(ec)) {
                if(!it->is_regular_file(ec))continue;
                auto ext=it->path().extension().string();
                std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
                if(ext!=".png"&&ext!=".txr"&&ext!=".aytex")continue;
                candidates.emplace_back(it->path().lexically_relative(root).generic_string(),it->path().string());
            }
            std::sort(candidates.begin(),candidates.end());
            if(candidates.empty())_properties->addWidget(label(L"No PNG or cooked textures in project textures"),25);
            else {
                constexpr size_t pageSize=8;
                const size_t pages=(candidates.size()+pageSize-1)/pageSize;
                _texturePage=std::min(_texturePage,pages-1);
                auto* paging=new HBox();paging->setSpacing(4);
                button(paging,L"‹",[this]{if(_texturePage)--_texturePage;_revision=0;},35);
                paging->addWidget(label(L"Textures "+std::to_wstring(_texturePage+1)+L" / "+std::to_wstring(pages)),0);
                button(paging,L"›",[this,pages]{if(_texturePage+1<pages)++_texturePage;_revision=0;},35);
                _properties->addWidget(paging,28);
                for(size_t n=_texturePage*pageSize;n<std::min(candidates.size(),(_texturePage+1)*pageSize);n+=2) {
                    auto* row=new HBox();row->setSpacing(5);
                    for(size_t j=n;j<std::min(n+2,candidates.size());++j) {
                        if(j>=(_texturePage+1)*pageSize)break;
                        row->addWidget(new ParticleTextureTile(candidates[j].first,candidates[j].second,_host,
                            [this,i](const std::string& ref){if(i<_document->asset().emitters.size())
                                apply([i,ref](auto& a){a.emitters[i].effect.texturePath=ref;});}),0);
                    }
                    _properties->addWidget(row,90);
                }
            }
        }
        _properties->addWidget(label(L"Source sheet / current base-FPS frame"),22);
        integerField(L"Atlas columns",f.columns,[](auto& e,uint32_t n) { e.columns=n; });
        integerField(L"Atlas rows",f.rows,[](auto& e,uint32_t n) { e.rows=n; });
        integerField(L"First frame",f.firstFrame,[](auto& e,uint32_t n) { e.firstFrame=n; });
        integerField(L"Frame count",f.frameCount,[](auto& e,uint32_t n) { e.frameCount=n; });
        numeric(L"Atlas FPS",f.framesPerSecond,[i](auto& a,float v){a.emitters[i].effect.framesPerSecond=v;});
        choice(L"Frame playback",{L"Loop",L"Once",L"Ping-pong"},int(f.animationPlayback),[i](auto& a,int n){a.emitters[i].effect.animationPlayback=static_cast<particle::AnimationPlayback>(n);},"particle_animation_playback");
        boolean(L"Frames over lifetime",f.animationOverLifetime,[i](auto& a){a.emitters[i].effect.animationOverLifetime=true;},[i](auto& a){a.emitters[i].effect.animationOverLifetime=false;});
        if(e.kind==particle::EffectLayerKind::Particles) {
        numeric(L"Particles/sec",f.rate,[i](auto& a,float v) { a.emitters[i].effect.rate=v; });
        numeric(L"Burst count",float(f.burst),[i](auto& a,float v) {
            if(v<0 || v>particle::ParticleEffect::kMaxCapacity || std::floor(v)!=v) throw std::invalid_argument("Burst needs a whole count");
            a.emitters[i].effect.burst=static_cast<uint32_t>(v);
        });
        numeric(L"Emit seconds",f.duration,[i](auto& a,float v) { a.emitters[i].effect.duration=v; });
        boolean(L"Repeat this emitter",f.looping,[i](auto& a) { a.emitters[i].effect.looping=true; },[i](auto& a) { a.emitters[i].effect.looping=false; });
        }
#define NUM(title,field) numeric(title,f.field,[i](auto& a,float v) { a.emitters[i].effect.field=v; })
        NUM(L"Life min",lifetime.min); NUM(L"Life max",lifetime.max);
        NUM(L"Size min",size.min); NUM(L"Size max",size.max);
        NUM(L"End size scale",endSizeScale);
        if(e.kind==particle::EffectLayerKind::Particles){NUM(L"Drag",drag);
        NUM(L"Speed X min",velocityMin.x); NUM(L"Speed X max",velocityMax.x);
        NUM(L"Speed Y min",velocityMin.y); NUM(L"Speed Y max",velocityMax.y);
        NUM(L"Speed Z min",velocityMin.z); NUM(L"Speed Z max",velocityMax.z);
        NUM(L"Gravity X",gravity.x); NUM(L"Gravity Y",gravity.y); NUM(L"Gravity Z",gravity.z);
        NUM(L"Extent X",extent.x); NUM(L"Extent Y",extent.y); NUM(L"Extent Z",extent.z);
        }
        NUM(L"Rotation min",rotation.min); NUM(L"Rotation max",rotation.max);
        NUM(L"Spin min",angularVelocity.min); NUM(L"Spin max",angularVelocity.max);
        if(f.facing==particle::ParticleFacing::Plane){NUM(L"Tilt X min",tiltX.min);NUM(L"Tilt X max",tiltX.max);NUM(L"Tilt Y min",tiltY.min);NUM(L"Tilt Y max",tiltY.max);NUM(L"Tumble X min",tumbleX.min);NUM(L"Tumble X max",tumbleX.max);NUM(L"Tumble Y min",tumbleY.min);NUM(L"Tumble Y max",tumbleY.max);}
        auto* colorTarget=new ComboBox();colorTarget->setId("particle_color_target");
        colorTarget->setItems({L"Start color",L"End color"});colorTarget->setSelectedIndex(_colorTarget);
        colorTarget->setOnSelectionChanged([this,alive](int n){if(*alive&&n>=0){_colorTarget=n;_revision=0;}});
        _properties->addWidget(colorTarget,28);
        auto* colors=new ColorPicker();colors->setId("particle_color_picker");
        const auto value=_colorTarget==0?f.startColor:f.endColor;
        colors->setColor({value.r,value.g,value.b,value.a},false);
        const int target=_colorTarget;
        colors->setOnColorCommitted([this,alive,i,target](const math::FVector4& color){
            if(!*alive)return;apply([i,target,color](auto& a){auto& f=a.emitters[i].effect;
                auto& c=target==0?f.startColor:f.endColor;c={color.x,color.y,color.z,color.w};});
        });
        _properties->addWidget(colors,190);
        NUM(L"Start alpha",startColor.a); NUM(L"End alpha",endColor.a);
#undef NUM
        integerField(L"Capacity",f.capacity,[](auto& e,uint32_t n) { e.capacity=n; });
        integerField(L"Seed",f.seed,[](auto& e,uint32_t n) { e.seed=n; });
        if(ui) ui->invalidateLayout();
    }
    std::shared_ptr<EditorParticleDocument> _document;
    IEditorHostServices& _host;
    EditorCommandButtons _commands;
    VBox* _root=nullptr; VBox* _properties=nullptr;VBox* _layers=nullptr;ParticleActivity* _activity=nullptr;
    TextLabel* _stats=nullptr; TextLabel* _message=nullptr; ParticleCanvas* _canvas=nullptr;
    std::shared_ptr<bool> _alive=std::make_shared<bool>(true);
    bool _ownsRoot=true;
    uint64_t _revision=0; size_t _selected=0;
    std::shared_ptr<ParticleCurveSource> _curveSource;
    authoring::CurveCanvas* _curveCanvas=nullptr;
    int _trackKind=0,_attribute=0,_sourceCurve=0,_colorTarget=0;size_t _sourceKey=0;
    bool _showTextureLibrary=false;size_t _texturePage=0;
    size_t _key=0;
};
}
bool registerEditorParticleExtension(EditorExtensionRegistry& registry,std::string* error) {
    EditorDescriptor descriptor; descriptor.id=kEditorParticleExtensionId;
    descriptor.displayName=L"Particle Effect"; descriptor.extensions={".pfx", ".ayparticle"};
    descriptor.assetTypes={"Particle Effect"}; descriptor.priority=80;
    descriptor.createDocument=[](const EditorOpenRequest& request,std::string& error) {
        auto document=std::make_shared<EditorParticleDocument>();
        return document->initialize(request,error)?std::static_pointer_cast<IEditorDocument>(document):nullptr;
    };
    descriptor.createView=[](const std::shared_ptr<IEditorDocument>& document,IEditorHostServices& host) -> std::unique_ptr<IEditorView> {
        auto particle=std::dynamic_pointer_cast<EditorParticleDocument>(document);
        return particle?std::make_unique<ParticleView>(std::move(particle),host):nullptr;
    };
    return registry.registerEditor(std::move(descriptor),error);
}
}
