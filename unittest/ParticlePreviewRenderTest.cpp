#include "../src/AYEditorParticlePreview.h"
#include <AYRenderer.h>
#include "detail/TextureImageLoader.h"
#include "detail/ScreenshotSidecar.h"
#include <AYResource/AssetPath.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
using namespace ayt;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
struct Fixture {
    HWND window=nullptr;
    render::Renderer renderer;
    render::UIRenderBackend ui;
    std::shared_ptr<editor::EditorParticleDocument> document=std::make_shared<editor::EditorParticleDocument>();
    editor::EditorParticleRuntimePreview preview{document};
    std::filesystem::path output;
    const render::RenderScene* mainScene=nullptr;
    explicit Fixture(std::filesystem::path path):output(std::move(path)) {
        std::filesystem::create_directories(output);
        window=CreateWindowExW(0,L"STATIC",L"Particle preview render test",WS_OVERLAPPED,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"hidden render test window");
        render::InitDesc init;init.windowHandle=window;init.width=init.height=128;
        init.vsync=false;init.msaa=0;init.backend=render::Backend::Direct3D11;
        require(renderer.initialize(init),"D3D11 renderer");require(ui.initialize(renderer),"UI render backend");
        ui.setFramebufferSize(128,128);std::string error;require(document->initialize({},error),"document initialize");
        auto& settings=document->previewSettings();settings.scene3D=true;settings.backend=particle::Backend::Cpu;
        settings.ground=false;settings.testObject=true;settings.distance=6;settings.yaw=0;settings.pitch=0;settings.focus={};
        particle::EffectAsset asset;asset.name="Preview occlusion";asset.duration=2;
        particle::EffectEmitter layer;layer.id=1;layer.offset={0,0,1};
        auto& e=layer.effect;e.dimension=particle::Dimension::ThreeD;e.facing=particle::ParticleFacing::Plane;
        e.rate=0;e.burst=1;e.capacity=1;e.looping=false;e.lifetime={10,10};e.size={3,3};
        e.velocityMin=e.velocityMax=e.gravity={};e.rotation=e.angularVelocity={0,0};
        e.startColor=e.endColor={1,0,0,1};e.endSizeScale=1;asset.emitters={layer};
        require(document->replaceAsset(asset,&error),error.c_str());
    }
    ~Fixture(){preview.shutdown();ui.shutdown();renderer.shutdown();if(window)DestroyWindow(window);}
    void frame(const std::string& capture={},math::FRectangle bounds={0,0,128,128}) {
        renderer.beginCompositeFrame({},128,128);ui.beginFrame();ui.beginCanvas({0,0,128,128});
        require(preview.render(ui,bounds),"runtime preview draw");ui.endCanvas();ui.endFrame();
        if(mainScene)renderer.render(*mainScene);
        if(!capture.empty())require(renderer.captureScreenshot((output/capture).string()),"capture test render");
        renderer.endFrame();
    }
    std::array<uint8_t,4> capture(const char* name) {
        const auto tga=output/(std::filesystem::path(name).stem().string()+".tga");
        std::filesystem::remove(tga);std::filesystem::remove(output/name);
        frame(name);
        for(int i=0;i<8;++i)frame();
        render::detail::finalizeScreenshotSidecar((output/std::filesystem::path(name).stem()).string());
        auto image=render::detail::decodeImageFile(tga.string());require(image.isValid(),"captured render pixels");
        const size_t center=(size_t(image.height/2)*image.width+image.width/2)*4;
        return {image.rgba8[center],image.rgba8[center+1],image.rgba8[center+2],image.rgba8[center+3]};
    }
};
}
int main(int argc,char** argv) {
    try {
        Fixture f(argc>1?argv[1]:"out/particle-preview-render");
        const auto blocked=f.capture("depth-blocked.png");
        f.document->previewSettings().testObject=false;const auto visible=f.capture("depth-visible.png");
        require(visible[0]>150&&visible[1]<25,"3D textured CPU particle renders red");
        require(blocked[0]<120&&blocked[1]>30,"test object depth occludes particle");
        require(f.document->preview()->liveParticles()==1,"CPU instance retained");
        auto asset=f.document->asset();asset.emitters[0].effect.dimension=particle::Dimension::TwoD;asset.emitters[0].effect.facing=particle::ParticleFacing::Billboard;
        std::string error;require(f.document->replaceAsset(asset,&error),"2D candidate");
        f.document->previewSettings().scene3D=false;f.document->previewSettings().backend=particle::Backend::Gpu;
        const auto gpu=f.capture("gpu-particle.png");require(f.preview.status().starts_with("GPU"),"2D uses actual GPU runtime");
        require(f.document->preview()==nullptr,"GPU preview has no CPU particle arrays");
        require(gpu[0]>150&&gpu[1]<25,"GPU compute precedes preview draw and UI sample");
        f.document->togglePreviewLayerHidden(0);const auto hidden=f.capture("gpu-hidden.png");
        require(hidden[0]<40&&hidden[1]<40,"GPU layer hiding masks draw only");
        f.document->togglePreviewLayerHidden(0);require(f.document->seek(.05f),"GPU seek request");
        for(int i=0;i<6;++i)f.frame();require(f.preview.status().find("seeking")==std::string::npos,"bounded asynchronous replay catches up");
        require(std::abs(f.document->previewTime()-.05)<1e-6,"GPU paused seek preserves requested time");
        f.document->stop();const auto stopped=f.capture("gpu-stop.png");require(stopped[0]<40,"GPU stop clears pixels");
        f.document->play();const auto restarted=f.capture("gpu-restart.png");require(restarted[0]>150,"GPU play after stop reseeds births");
        f.document->previewSettings().scene3D=true;f.frame();require(f.preview.status().find("CPU fallback")!=std::string::npos,"3D diagnoses CPU fallback");
        require(f.document->preview()->liveParticles()>0,"GPU to CPU handoff restores isolated samples");
        f.frame({}, {0,0,96,96});f.frame();
        // A second target in the same frame must receive distinct compute/draw views.
        editor::EditorParticleRuntimePreview second(f.document);
        f.renderer.beginCompositeFrame({},128,128);f.ui.beginFrame();f.ui.beginCanvas({0,0,128,128});
        require(f.preview.render(f.ui,{0,0,64,128}),"first preview target");require(second.render(f.ui,{64,0,128,128}),"second preview target");
        f.ui.endCanvas();f.ui.endFrame();f.renderer.endFrame();second.shutdown();
        // Actual authored examples, including a source PNG atlas.
        auto assets=std::filesystem::absolute("AYRuntime/AYParticle/examples/FallingLeaves/Assets");
        resource::setAssetRoot(assets.string());
        require(f.document->initialize({(assets/"effects/GpuRain.ayparticle").string()},error),error.c_str());
        f.document->previewSettings().scene3D=false;f.document->previewSettings().backend=particle::Backend::Gpu;
        f.document->play();f.document->tick(.25f);for(int i=0;i<20;++i)f.frame();
        f.capture("gpu-rain.png");require(f.preview.status().starts_with("GPU"),"GPU rain example uses GPU");
        auto rain=render::detail::decodeImageFile((f.output/"gpu-rain.tga").string());size_t drops=0;
        for(size_t i=0;i<rain.rgba8.size();i+=4)if(rain.rgba8[i+2]>rain.rgba8[i]+25)++drops;
        require(drops>50,"GPU rain produces visible blue drops");
        require(f.document->preview()==nullptr,"GPU rain never retains CPU particles");
        require(f.document->initialize({(assets/"effects/FallingLeaves.ayparticle").string()},error),error.c_str());
        f.document->previewSettings().scene3D=false;f.document->play();f.document->tick(.25f);
        for(int i=0;i<20;++i)f.frame();f.capture("gpu-leaves.png");
        require(f.preview.status().starts_with("GPU"),"source PNG leaf atlas uses real GPU preview");
        require(f.document->initialize({(assets/"effects/TumblingLeaves3D.ayparticle").string()},error),error.c_str());
        require(f.document->seek(1),"3D leaf pose seek");f.frame();f.preview.focus();f.capture("cpu-leaves-3d.png");
        require(f.preview.status().find("CPU fallback")!=std::string::npos,"3D atlas tumble uses CPU/depth preview");
        require(f.document->preview()->liveParticles()>0,"3D leaves are live");
        // Main-scene Bloom (26..30) and exposure (31) must not overwrite the
        // preview's compute/FBO/view matrices populated earlier in this frame.
        require(f.document->replaceAsset(asset,&error),"restore 2D test card");
        f.document->previewSettings().scene3D=false;f.document->previewSettings().backend=particle::Backend::Gpu;
        f.document->previewSettings().focus={};f.document->previewSettings().screenHeight=7;
        f.renderer.configurePipeline(render::RenderPipelineDesc::makeDeferred());
        f.renderer.setPostProcessBloomStrength(.5f);f.renderer.setAutoExposureEnabled(true);
        f.renderer.setMainCameraLookAtPerspective({4,3,-5},{0,0,0},{0,1,0},50,1,.1f,100);
        const auto mainEye=f.renderer.mainCameraPosition();
        const char* glowSource=R"(
material PreviewCompatibilityGlow {
    uniform vec4 baseColor
    vertex { in pos : position return modelViewProjection * vec4(pos.x,pos.y,pos.z,1.0) }
    fragment { return baseColor }
}
)";
        auto glow=f.renderer.createMaterialFromPhoskia(glowSource,"particle-preview-compatibility");
        f.renderer.setMaterialColor(glow,"baseColor",4,4,4,1);
        f.renderer.setMaterialModel(glow,render::MaterialModel::Unlit);
        auto mesh=f.renderer.createUnitCube();render::RenderScene mainScene;mainScene.add(mesh,glow);
        f.mainScene=&mainScene;const auto composite=f.capture("gpu-preview-with-bloom.png");
        require(composite[0]>150&&composite[1]<25,"Bloom/exposure coexist with preview GPU compute and target");
        const auto afterEye=f.renderer.mainCameraPosition();
        require(afterEye.x==mainEye.x&&afterEye.y==mainEye.y&&afterEye.z==mainEye.z,"preview preserves main camera");
        const auto submitted=[&](const char* name){
            for(const auto& pass:f.renderer.getFrameStats().passes)if(pass.name==name&&pass.drawCalls>0)return true;
            return false;
        };
        require(submitted("BloomBlur")&&submitted("AutoExposure"),"main scene actually submits Bloom and exposure");
        f.mainScene=nullptr;f.renderer.destroyMesh(mesh);f.renderer.destroyMaterial(glow);
        std::cout<<"PASS D3D11 particle editor: perspective/depth, CPU/GPU handoff, no CPU arrays, visibility, seek, stop/restart, resize, multiple targets, GPU rain, source atlas, 3D leaves, main Bloom/exposure coexistence\n";
        return 0;
    } catch(const std::exception& ex) {std::cerr<<"FAIL: "<<ex.what()<<'\n';return 1;}
}
