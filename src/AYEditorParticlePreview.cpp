#include "AYEditorParticlePreview.h"
#include <AYRenderer.h>
#include <AYParticle/ParticleGeometry.h>
#include <AYResource/AssetPath.h>
#include <AYMath/MathUtils.h>
#ifdef AYEDITOR_PARTICLE_RUNTIME_PREVIEW
#include "ParticleGpuState.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>
namespace ayt::editor {
namespace {
constexpr const char* environmentSource=R"(
material ParticlePreviewEnvironment {
    uniform vec4 baseColor
    vertex { in pos : position return modelViewProjection * vec4(pos.x,pos.y,pos.z,1.0) }
    fragment { return baseColor }
}
)";
math::FVector3 vector(particle::Vec3 p) {return {p.x,p.y,p.z};}
particle::Vec3 vector(math::FVector3 p) {return {p.x,p.y,p.z};}
}
struct EditorParticleRuntimePreview::Impl {
    std::shared_ptr<EditorParticleDocument> document;
    render::UIRenderBackend* ui=nullptr;
    render::Renderer* renderer=nullptr;
    ui::IRenderBackend::RenderTargetHandle target{};
    int width=0,height=0;
    render::MeshHandle quad{},cube{};
    render::TextureHandle disc{};
    render::MaterialHandle ground{},object{},grid{};
    std::unordered_map<std::string,render::MaterialHandle> materials;
    struct Batch {render::ParticleDrawData data;render::DrawPayload2D payload;};
    std::deque<Batch> batches;
#ifdef AYEDITOR_PARTICLE_RUNTIME_PREVIEW
    std::unique_ptr<entity::ParticleGpuState> gpu;
#endif
    particle::PlaybackControl control;
    uint64_t epoch=UINT64_MAX;
    double renderedTime=0;
    bool replaying=false;
    std::string status="CPU";
    explicit Impl(std::shared_ptr<EditorParticleDocument> d):document(std::move(d)) {}
    render::MaterialHandle environment(const char* name,math::FVector4 color) {
        auto m=renderer->createMaterialFromPhoskia(environmentSource,
            "particle-preview:"+std::to_string(reinterpret_cast<uintptr_t>(this))+":"+name);
        renderer->setMaterialColor(m,"baseColor",color.x,color.y,color.z,color.w);
        renderer->setMaterialModel(m,render::MaterialModel::Unlit);
        renderer->setMaterialSurfaceProperties(m,0,0,true);
        return m;
    }
    render::MaterialHandle material(const particle::ParticleEffect& effect) {
        const auto path=effect.texturePath.empty()?std::string{}:
            resource::resolveAssetPath(document->path(),effect.texturePath);
        const auto key=path+":"+std::to_string(int(effect.blend));
        if(auto found=materials.find(key);found!=materials.end()) return found->second;
        render::TextureHandle texture;
        if(!path.empty()) texture=renderer->loadTexture(path);
        else {
            if(!disc.isValid()) {
                uint8_t pixels[32*32*4];
                for(int y=0;y<32;++y)for(int x=0;x<32;++x) {
                    const int i=(y*32+x)*4;
                    const float dx=(x+.5f)/16-1,dy=(y+.5f)/16-1;
                    pixels[i]=pixels[i+1]=pixels[i+2]=255;
                    pixels[i+3]=uint8_t(255*std::clamp(1-dx*dx-dy*dy,0.f,1.f));
                }
                disc=renderer->createTextureFromRgba8(32,32,pixels);
            }
            texture=disc;
        }
        if(!texture.isValid()) {status="CPU: missing texture "+path;return {};}
        auto m=renderer->createMaterialFromPhoskia(render::kParticlePhoskiaSource,
            "particle-preview:"+std::to_string(reinterpret_cast<uintptr_t>(this))+":"+key);
        renderer->setMaterialTexture(m,"albedoMap",texture);
        renderer->setMaterialBlendMode(m,effect.blend==particle::Blend::Additive?
            render::BlendMode::Additive:render::BlendMode::Alpha);
        renderer->setMaterialSurfaceProperties(m,2,0,true);
        renderer->setMaterialModel(m,render::MaterialModel::Unlit);
        if(m.isValid()) materials.emplace(key,m);
        else status="CPU: particle material unavailable";
        return m;
    }
    void appendCpu(render::RenderScene& scene,const render::PreviewSceneCamera& camera) {
        const auto* preview=document->preview();if(!preview)return;
        const auto& settings=document->previewSettings();
        const auto right=vector(math::FVector3{camera.view(0,0),camera.view(0,1),camera.view(0,2)});
        const auto up=vector(math::FVector3{camera.view(1,0),camera.view(1,1),camera.view(1,2)});
        for(size_t i=0;i<preview->instances().size();++i) {
            if(!document->previewLayerVisible(i))continue;
            const auto& instance=preview->instances()[i];const auto& effect=instance.effect();
            auto m=material(effect);if(!m.isValid())continue;
            std::vector<particle::ParticleSample> samples;
            samples.reserve(instance.particles().size());
            for(const auto& p:instance.particles())samples.push_back(instance.sample(p,preview->emitterPose(i)));
            if(settings.scene3D&&effect.blend==particle::Blend::Alpha) {
                const auto distance=[&](const auto& s){const auto v=vector(s.position)-camera.position;return v.x*v.x+v.y*v.y+v.z*v.z;};
                std::stable_sort(samples.begin(),samples.end(),[&](const auto& a,const auto& b){return distance(a)>distance(b);});
            }
            for(size_t start=0;start<samples.size();start+=16383) {
                const auto subset=std::span<const particle::ParticleSample>(samples).subspan(start,std::min<size_t>(16383,samples.size()-start));
                auto g=particle::buildGeometry(subset,effect,settings.scene3D?right:particle::Vec3{1,0,0},settings.scene3D?up:particle::Vec3{0,1,0});
                auto& b=batches.emplace_back();b.data.indices=std::move(g.indices);
                b.data.vertices.resize(g.vertices.size());
                static_assert(sizeof(particle::Vertex)==sizeof(render::ParticleVertex));
                std::memcpy(b.data.vertices.data(),g.vertices.data(),g.vertices.size()*sizeof(particle::Vertex));
                render::DrawItem item;item.mesh=quad;item.material=m;item.particleBatch=&b.data;
                item.shadowFlags=render::ShadowFlags::None;
                if(!settings.scene3D) {b.payload.packedSortKey=uint32_t(i);item.payload=&b.payload;}
                else {
                    particle::Vec3 center{};for(const auto& s:subset)center=center+s.position;
                    center=center*(1.f/float(subset.size()));item.world=math::Float4x4::translation(vector(center));
                    for(auto& v:b.data.vertices){v.x-=center.x;v.y-=center.y;v.z-=center.z;}
                }
                scene.add(item);
            }
        }
    }
    void appendEnvironment(render::RenderScene& scene) {
        const auto& settings=document->previewSettings();if(!settings.scene3D)return;
        if(!cube.isValid())cube=renderer->createUnitCube();
        if(!ground.isValid())ground=environment("ground",{.085f,.11f,.15f,1});
        if(!object.isValid())object=environment("object",{.27f,.34f,.43f,1});
        if(!grid.isValid())grid=environment("grid",{.15f,.2f,.27f,1});
        const auto add=[&](render::MaterialHandle m,math::FVector3 p,math::FVector3 size){
            render::DrawItem item;item.mesh=cube;item.material=m;
            item.world=math::Float4x4::translation(p)*math::Float4x4::scaling(size);
            item.shadowFlags=render::ShadowFlags::None;scene.add(item);
        };
        if(settings.ground) {
            add(ground,{0,-1.08f,0},{20,.1f,20});
            for(int n=-10;n<=10;++n) {add(grid,{float(n),-1.02f,0},{.008f,.01f,20});add(grid,{0,-1.02f,float(n)},{20,.01f,.008f});}
        }
        if(settings.testObject)add(object,{0,-.25f,0},{1.5f,1.5f,1.5f});
    }
};
EditorParticleRuntimePreview::EditorParticleRuntimePreview(std::shared_ptr<EditorParticleDocument> d):_impl(std::make_unique<Impl>(std::move(d))) {}
EditorParticleRuntimePreview::~EditorParticleRuntimePreview(){shutdown();}
const std::string& EditorParticleRuntimePreview::status() const noexcept{return _impl->status;}
void EditorParticleRuntimePreview::shutdown() {
    auto& p=*_impl;
#ifdef AYEDITOR_PARTICLE_RUNTIME_PREVIEW
    p.gpu.reset();
#endif
    p.document->setPreviewReplayPending(false);
    p.document->setRuntimeGpuActive(false);
    if(p.ui&&p.ui->isInitialized())p.ui->releaseRenderTarget(p.target);
    if(p.renderer&&p.renderer->isInitialized()) {
        for(auto& [key,m]:p.materials)p.renderer->destroyMaterial(m);
        p.renderer->destroyMaterial(p.ground);p.renderer->destroyMaterial(p.grid);p.renderer->destroyMaterial(p.object);
        p.renderer->destroyMesh(p.quad);p.renderer->destroyMesh(p.cube);p.renderer->destroyTexture(p.disc);
    }
    p.materials.clear();p.ui=nullptr;p.renderer=nullptr;p.target={};p.width=p.height=0;p.epoch=UINT64_MAX;
    p.quad={};p.cube={};p.disc={};p.ground={};p.grid={};p.object={};
}
void EditorParticleRuntimePreview::focus() {
    auto& p=*_impl;particle::Vec3 lo{},hi{};bool any=false;
    const auto include=[&](particle::Vec3 v,float r){
        const particle::Vec3 a{v.x-r,v.y-r,v.z-r},b{v.x+r,v.y+r,v.z+r};
        if(!any){lo=a;hi=b;any=true;return;}
        lo={std::min(lo.x,a.x),std::min(lo.y,a.y),std::min(lo.z,a.z)};
        hi={std::max(hi.x,b.x),std::max(hi.y,b.y),std::max(hi.z,b.z)};
    };
    if(p.document->preview()) for(size_t i=0;i<p.document->preview()->instances().size();++i) {
        if(!p.document->previewLayerVisible(i))continue;
        const auto& e=p.document->preview()->instances()[i];
        for(const auto& particle:e.particles()) {const auto s=e.sample(particle,p.document->preview()->emitterPose(i));include(s.position,s.size*.5f);}
    }
    // GPU focus uses authored bounds, never synchronous particle readback.
    if(!any)for(size_t i=0;i<p.document->asset().emitters.size();++i) {
        if(!p.document->previewLayerVisible(i))continue;
        const auto& layer=p.document->asset().emitters[i];const auto e=particle::layerEffect(layer);
        const auto maximum=[](const particle::NumberProperty& source,float legacy){
            if(source.mode==particle::NumberMode::Legacy)return legacy;
            if(source.mode==particle::NumberMode::Constant)return source.value;
            if(source.mode==particle::NumberMode::Uniform)return std::max(std::abs(source.min),std::abs(source.max));
            float value=0;for(const auto& choice:source.choices)if(choice.weight>0)value=std::max(value,std::abs(choice.value));
            for(const auto* keys:{&source.curve,&source.upperCurve})for(const auto& key:*keys)value=std::max(value,std::abs(key.value));
            return value;
        };
        const float life=maximum(e.value(particle::FloatAttribute::Lifetime),e.lifetime.max);
        const float size=maximum(e.value(particle::FloatAttribute::Size),e.size.max)
            *maximum(e.value(particle::FloatAttribute::SizeMultiplier),std::max(1.f,e.endSizeScale));
        const auto extent=e.shape==particle::Shape::Point?particle::Vec3{}:e.extent;
        const float sway=maximum(e.value(particle::FloatAttribute::SwayAmplitude),0);
        // Sum independent displacement maxima; drag only shrinks these bounds.
        const particle::Vec3 radius{extent.x+std::max(std::abs(e.velocityMin.x),std::abs(e.velocityMax.x))*life+std::abs(e.gravity.x)*life*life*.5f+sway+size,
            extent.y+std::max(std::abs(e.velocityMin.y),std::abs(e.velocityMax.y))*life+std::abs(e.gravity.y)*life*life*.5f+size,
            extent.z+std::max(std::abs(e.velocityMin.z),std::abs(e.velocityMax.z))*life+std::abs(e.gravity.z)*life*life*.5f+size};
        include(layer.offset-radius,0);include(layer.offset+radius,0);
    }
    auto& s=p.document->previewSettings();s.focus=(lo+hi)*.5f;
    const float extent=std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z,1.f});
    s.screenHeight=std::clamp(extent*1.4f,.1f,2000.f);s.distance=std::clamp(extent*1.8f,.2f,3000.f);
}
bool EditorParticleRuntimePreview::render(render::UIRenderBackend& ui,const math::FRectangle& bounds) {
    auto& p=*_impl;auto* renderer=ui.previewRenderer();if(!renderer)return false;
    if(p.ui!=&ui) {shutdown();p.ui=&ui;p.renderer=renderer;}
    const int w=std::clamp(int(std::ceil((bounds.maxX-bounds.minX)*ui.getUiScale())),1,4096);
    const int h=std::clamp(int(std::ceil((bounds.maxY-bounds.minY)*ui.getUiScale())),1,4096);
    ui::IRenderBackend::RenderTargetDesc desc;desc.width=w;desc.height=h;desc.hasAlpha=false;
    if(!p.target.isValid())p.target=ui.createRenderTarget(desc);
    else if(w!=p.width||h!=p.height) {if(!ui.resizeRenderTarget(p.target,desc)){p.status="Preview target allocation failed";return false;}}
    if(!p.target.isValid()){p.status="Preview target unavailable";return false;}p.width=w;p.height=h;
    if(!p.quad.isValid())p.quad=renderer->createUnitQuad();
    auto& settings=p.document->previewSettings();
    render::PreviewSceneCamera camera;
    const float aspect=float(w)/h;
    if(settings.scene3D) {
        const particle::Vec3 offset{std::sin(settings.yaw)*std::cos(settings.pitch),std::sin(settings.pitch),-std::cos(settings.yaw)*std::cos(settings.pitch)};
        camera.position=vector(settings.focus+offset*settings.distance);
        camera.view=math::lh::lookAt(camera.position,vector(settings.focus),{0,1,0});
        camera.projection=math::lh::perspective(math::radians(50.f),aspect,.02f,10000.f);
    } else {
        camera.position=vector(settings.focus+particle::Vec3{0,0,-10});
        camera.view=math::lh::lookAt(camera.position,vector(settings.focus),{0,1,0});
        camera.projection=math::lh::ortho(-settings.screenHeight*aspect*.5f,settings.screenHeight*aspect*.5f,
            -settings.screenHeight*.5f,settings.screenHeight*.5f,.01f,10000.f);
    }
    render::RenderScene scene;p.batches.clear();
    const bool rendered=ui.renderScenePreview(p.target,scene,camera,settings.background,[&](uint16_t computeView){
        bool useGpu=false;
#ifdef AYEDITOR_PARTICLE_RUNTIME_PREVIEW
        if(!p.gpu)p.gpu=std::make_unique<entity::ParticleGpuState>();
        p.gpu->beginFrame();
        auto asset=p.document->asset();asset.seed=p.document->previewSeed();
        for(auto& layer:asset.emitters)if(!layer.effect.texturePath.empty())
            layer.effect.texturePath=resource::resolveAssetPath(p.document->path(),layer.effect.texturePath);
        if(settings.scene3D) p.status=settings.backend==particle::Backend::Cpu?"CPU · perspective / depth":"CPU fallback: 3D preview requires CPU";
        else useGpu=p.gpu->select(1,asset,int(settings.backend),particle::EffectAsset::kMaxTotalCapacity,p.status);
        p.gpu->endFrame();
#else
        p.status=settings.backend==particle::Backend::Cpu?"CPU":"CPU fallback: GPU integration is not built";
#endif
        p.document->setRuntimeGpuActive(useGpu);
        p.appendEnvironment(scene);
        if(useGpu) {
#ifdef AYEDITOR_PARTICLE_RUNTIME_PREVIEW
            if(p.epoch!=p.document->previewEpoch()) {
                p.epoch=p.document->previewEpoch();p.renderedTime=0;p.control.play();
                p.replaying=p.document->previewTime()>0;
                if(p.document->previewStopped())p.control.stop(true);
            }
            const double remaining=std::max(0.0,p.document->previewTime()-p.renderedTime);
            const float dt=float(std::min(remaining,p.replaying?1.0/60:.25));
            p.control.paused=remaining<=1e-7&&p.document->previewStopped();
            // A zero-time first frame must resolve pending bursts even when the
            // document is paused. Later frames use dt=0, keeping births frozen.
            p.gpu->update(1,dt,{},p.control,computeView);p.renderedTime+=dt;
            const bool pending=p.document->previewTime()-p.renderedTime>1e-5;
            p.document->setPreviewReplayPending(pending);p.replaying=pending;
            if(!p.control.playing&&!pending&&!p.document->previewStopped())p.document->finishRuntimePreview();
            auto draws=p.gpu->draws(1);
            for(size_t i=0;i<draws.size();++i) {
                if(!p.document->previewLayerVisible(i))continue;
                auto& b=p.batches.emplace_back();b.data=std::move(draws[i]);b.payload.packedSortKey=uint32_t(i);
                render::DrawItem item;item.particleBatch=&b.data;item.payload=&b.payload;
                item.shadowFlags=render::ShadowFlags::None;scene.add(item);
            }
            p.status="GPU · "+std::to_string(draws.size())+" emitters · no CPU particle arrays";
            if(pending)p.status+=" · seeking "+std::to_string(p.renderedTime)+" / "+std::to_string(p.document->previewTime())+" s";
#endif
        } else {p.epoch=UINT64_MAX;p.document->setPreviewReplayPending(false);p.appendCpu(scene,camera);}
    });
    if(!rendered) {p.status="Preview suspended: offscreen view budget/paint context";return false;}
    ui.blitRenderTarget(p.target,bounds);return true;
}
}
