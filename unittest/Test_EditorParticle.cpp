#ifdef AYEDITOR_HAS_PARTICLE
#include <AYEditor/EditorParticleDocument.h>
#include <AYEditor/EditorProjectAssetFactory.h>
#include <AYEditor/EditorAssetTilePresenter.h>
#include <AYEditor/EditorAssetDeleteAnalysis.h>
#include <AYIO/File.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYUI/Button.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ColorPicker.h>
#include <AYUI/Authoring/CurveCanvas.h>
#include "../src/AYEditorParticleCurveSource.h"
#include <AYTest.h>
#include <chrono>
#include <filesystem>
using namespace ayt::editor;
namespace {
struct Fixture {
    std::filesystem::path root=ayt::test::testTmpDir()/("particle-editor-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::shared_ptr<EditorParticleDocument> document=std::make_shared<EditorParticleDocument>();
    Fixture() { std::filesystem::create_directory(root); std::string error; if(!document->initialize({},error)) throw std::runtime_error(error); }
};
struct Host : IEditorHostServices, IEditorDocumentSavePathProvider {
    EditorWorkspace value; std::string root;
    EditorWorkspace& workspace() noexcept override { return value; }
    const std::string& projectRoot() const noexcept override { return root; }
    void requestRepaint() override {}
    void setStatusText(const std::wstring&) override {}
    std::string chooseDocumentSavePath(const IEditorDocument&,bool) override { return (std::filesystem::path(root)/"from-ui.ayparticle").string(); }
};
ayt::ui::Widget* find(ayt::ui::Widget* root,const std::string& id) {
    if(root->getId()==id) return root;
    for(auto* child:root->getChildren()) if(auto* result=find(child,id)) return result;
    return nullptr;
}
void click(ayt::ui::Widget* root,const char* id) {
    auto* b=dynamic_cast<ayt::ui::Button*>(find(root,id)); CHECK_NOT_NULL(b); if(!b) return;
    const auto bounds=b->getWorldBounds();
    b->onMouseButtonDown({{bounds.minX+2,bounds.minY+2},0});
    b->onMouseButtonUp({{bounds.minX+2,bounds.minY+2},0});
}
}
TEST_SUITE(AYEditor_Particle)
TEST_CASE(runtime_handoff_and_view_settings_preserve_content_and_transport) {
    Fixture f;const auto asset=f.document->asset();const auto revision=f.document->revision();
    f.document->play();f.document->tick(.2f);const auto before=f.document->previewTime();
    f.document->setRuntimeGpuActive(true);
    CHECK(f.document->preview()==nullptr);CHECK(f.document->playing());
    CHECK(std::abs(f.document->previewTime()-before)<1e-6);
    f.document->tick(.1f);CHECK(f.document->previewTime()>before);
    f.document->setPreviewReplayPending(true);const auto pending=f.document->previewTime();
    f.document->tick(.1f);CHECK(f.document->previewTime()==pending);
    f.document->setPreviewReplayPending(false);f.document->pause();f.document->tick(.1f);
    CHECK(f.document->previewTime()==pending);f.document->step();CHECK(f.document->previewTime()>pending);
    const auto epoch=f.document->previewEpoch();CHECK(f.document->seek(.5f));
    CHECK(f.document->previewEpoch()>epoch);CHECK(std::abs(f.document->previewTime()-.5)<1e-6);
    CHECK(f.document->preview()==nullptr);
    f.document->setRuntimeGpuActive(false);CHECK(f.document->preview()->liveParticles()>0);
    CHECK(std::abs(f.document->previewTime()-.5)<1e-5);
    auto& settings=f.document->previewSettings();settings.scene3D=true;settings.backend=ayt::particle::Backend::Gpu;
    settings.background=0xffffffff;settings.ground=false;settings.testObject=false;
    f.document->togglePreviewLayerSolo(1);CHECK_FALSE(f.document->previewLayerVisible(0));CHECK(f.document->previewLayerVisible(1));
    f.document->togglePreviewLayerSolo(1);CHECK(f.document->previewLayerVisible(0));
    f.document->togglePreviewLayerHidden(1);CHECK_FALSE(f.document->previewLayerVisible(1));
    CHECK(f.document->asset()==asset);CHECK(f.document->revision()==revision);CHECK_FALSE(f.document->isDirty());
    f.document->stop();f.document->setRuntimeGpuActive(true);CHECK(f.document->previewStopped());
    CHECK(f.document->previewTime()==0);f.document->play();CHECK_FALSE(f.document->previewStopped());
    f.document->finishRuntimePreview();f.document->play();CHECK(f.document->playing());CHECK_FALSE(f.document->previewStopped());
}
TEST_CASE(layer_visibility_follows_stable_ids_after_reorder_and_rename) {
    Fixture f;auto asset=f.document->asset();for(size_t i=0;i<asset.emitters.size();++i)asset.emitters[i].id=uint32_t(i+1);
    CHECK(f.document->replaceAsset(asset));f.document->togglePreviewLayerHidden(1);
    std::swap(asset.emitters[0],asset.emitters[1]);asset.emitters[0].name="Renamed";
    CHECK(f.document->replaceAsset(asset));CHECK_FALSE(f.document->previewLayerVisible(0));CHECK(f.document->previewLayerVisible(1));
}

TEST_CASE(transport_preview_isolated_and_does_not_dirty) {
    Fixture f; CHECK_FALSE(f.document->isDirty());
    CHECK(f.document->preview()->liveParticles()==61);
    f.document->play(); f.document->tick(0.2f);
    CHECK(f.document->preview()->time()>0.19);
    CHECK(!f.document->preview()->instances()[2].particles().empty());
    f.document->pause(); const auto time=f.document->preview()->time(); f.document->tick(1);
    CHECK(f.document->preview()->time()==time);
    f.document->step(); CHECK(f.document->preview()->time()>time);
    CHECK_FALSE(f.document->playing()); CHECK_FALSE(f.document->isDirty());
    f.document->stop(); CHECK(f.document->preview()->liveParticles()==0);
    f.document->play(); CHECK(f.document->preview()->time()==0);
    f.document->restart(); CHECK(f.document->playing());
    CHECK(f.document->preview()->time()==0); CHECK_FALSE(f.document->isDirty());
}
TEST_CASE(composition_history_save_reopen_and_recovery) {
    Fixture f; auto asset=f.document->asset(); asset.emitters[1].offset={2,3,0}; asset.emitters[2].delay=0.25f;
    asset.backend=ayt::particle::Backend::Gpu;
    CHECK(f.document->replaceAsset(asset)); CHECK(f.document->isDirty());
    const auto path=(f.root/"saved.ayparticle").string(); CHECK(f.document->saveAs(path)); CHECK_FALSE(f.document->isDirty());
    CHECK(f.document->undo()); CHECK(f.document->isDirty());
    CHECK(f.document->redo()); CHECK_FALSE(f.document->isDirty());
    auto invalid=asset; invalid.emitters.clear();
    CHECK_FALSE(f.document->replaceAsset(invalid)); CHECK(f.document->asset()==asset);
    auto edited=asset; edited.name="Edited"; CHECK(f.document->replaceAsset(edited));
    const auto recovery=(f.root/"recovery.ayparticle").string();
    CHECK(f.document->writeRecoveryCopy(recovery)); CHECK(f.document->path()==path); CHECK(f.document->isDirty());
    EditorParticleDocument reopened; std::string error;
    CHECK(reopened.initialize({path},error)); CHECK(reopened.asset()==asset);
    CHECK(f.document->reload()); CHECK_FALSE(f.document->isDirty()); CHECK(f.document->asset()==asset);
}
TEST_CASE(project_asset_creation_classification_and_extension_routing) {
    Fixture f; const auto first=createEditorProjectAsset(f.root.string(),EditorAssetType::ParticleEffect);
    const auto second=createEditorProjectAsset(f.root.string(),EditorAssetType::ParticleEffect);
    CHECK(first && second); CHECK(first.absolutePath!=second.absolutePath);
    CHECK(classifyEditorAssetPath(first.absolutePath)==EditorAssetType::ParticleEffect);
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("test.ayparticle"));
    EditorExtensionRegistry registry; CHECK(registerEditorParticleExtension(registry));
    EditorOpenRequest request; request.resourcePath=first.absolutePath;
    const auto* descriptor=registry.resolve(request); CHECK_NOT_NULL(descriptor);
    CHECK(descriptor && descriptor->id==kEditorParticleExtensionId);
    EditorAssetDatabase database; CHECK(database.open(f.root.string())); CHECK(database.scanNow());
    bool found=false; for(const auto& record:database.records()) if(record.type==EditorAssetType::ParticleEffect) found=true;
    CHECK(found);
    std::filesystem::create_directory(f.root/"Assets/textures");
    const auto texture=(f.root/"Assets/textures/marker.png").string();
    CHECK(ayt::io::File::atomicWrite(texture,"PNG",3));
    auto asset=ayt::particle::combinedExplosion(); asset.emitters[0].effect.texturePath="textures/marker.png";
    CHECK(ayt::particle::saveEffectAsset(first.absolutePath,asset));
    CHECK(database.scanNow()); EditorAssetId textureId=0;
    for(const auto& record:database.records()) if(record.name=="marker.png") textureId=record.id;
    CHECK(textureId!=0);
    const auto analysis=analyzeEditorAssetDeletion(database,{textureId});
    CHECK(analysis.references.size()==1);
}
TEST_CASE(view_buttons_fields_and_shutdown_share_document_history) {
    Fixture f; Host host; host.root=f.root.string();
    EditorExtensionRegistry registry; CHECK(registerEditorParticleExtension(registry));
    const auto* descriptor=registry.find(kEditorParticleExtensionId); CHECK_NOT_NULL(descriptor);
    auto view=descriptor->createView(f.document,host); CHECK_NOT_NULL(view);
    auto* root=view->rootWidget();
    click(root,"command_particle.play"); view->tick(0.1f); CHECK(f.document->playing());
    click(root,"command_particle.pause"); view->tick(0); CHECK_FALSE(f.document->playing());
    auto* input=dynamic_cast<ayt::ui::TextInput*>(find(root,"particle_effect_name")); CHECK_NOT_NULL(input);
    input->setFocus(true); input->setText(L"UI Edited"); input->onKeyDown(ayt::ui::UIKey_Enter);
    view->tick(0); CHECK(f.document->asset().name=="UI Edited"); CHECK(f.document->isDirty());
    click(root,"command_edit.undo"); view->tick(0); CHECK_FALSE(f.document->isDirty());
    click(root,"command_edit.redo"); view->tick(0); CHECK(f.document->isDirty());
    click(root,"command_file.save"); view->tick(0); CHECK_FALSE(f.document->isDirty());
    CHECK(std::filesystem::exists(f.document->path()));
    click(root,"command_file.save_as"); view->tick(0); CHECK_FALSE(f.document->isDirty());
    click(root,"command_particle.step"); view->tick(0); CHECK(f.document->preview()->time()>0);
    view->prepareForUiShutdown();
    click(root,"command_particle.restart"); CHECK_FALSE(f.document->playing());
}
TEST_CASE(scene_backend_choice_supports_undo_save_and_cpu_reference_preview) {
    Fixture f; Host host; host.root=f.root.string();
    EditorExtensionRegistry registry; CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);
    auto* root=view->rootWidget();
    auto* choice=dynamic_cast<ayt::ui::ComboBox*>(find(root,"particle_scene_backend"));
    CHECK_NOT_NULL(choice); if(!choice) return;
    choice->setSelectedIndexAndNotify(1); view->tick(0);
    CHECK(f.document->asset().backend==ayt::particle::Backend::Gpu); CHECK(f.document->isDirty());
    click(root,"command_edit.undo"); view->tick(0);
    CHECK(f.document->asset().backend==ayt::particle::Backend::Cpu);
    click(root,"command_edit.redo"); view->tick(0);
    click(root,"command_file.save"); view->tick(0); CHECK_FALSE(f.document->isDirty());
    EditorParticleDocument reopened; std::string error; CHECK(reopened.initialize({f.document->path()},error));
    CHECK(reopened.asset().backend==ayt::particle::Backend::Gpu);
    click(root,"command_particle.play"); view->tick(.1f);
    CHECK(f.document->preview()->liveParticles()>0);
}
TEST_CASE(lifetime_draft_commits_once_cancels_and_rejects_stale_document) {
    Fixture f;
    const auto original=f.document->asset();
    ParticleCurveSource source(f.document,1,0);
    auto snapshot=source.curveTrack("lifetime");
    CHECK(source.beginEdit("Size gesture"));
    std::string key="1";
    CHECK(source.updateKey(key,0.4,{2})); CHECK(source.updateKey(key,0.6,{3}));
    CHECK(f.document->asset()==original); CHECK_FALSE(f.document->isDirty());
    CHECK(source.endEdit(false)); CHECK(f.document->isDirty());
    CHECK(f.document->asset().emitters[1].effect.sizeCurve[1].value==3);
    CHECK(snapshot->keys[1].values[0]==original.emitters[1].effect.sizeCurve[1].value);
    CHECK(f.document->undo()); CHECK(f.document->asset()==original);
    CHECK(f.document->redo()); const auto edited=f.document->asset();
    CHECK(source.beginEdit("Cancelled gesture")); CHECK(source.updateKey(key,0.5,{4}));
    CHECK(source.endEdit(true)); CHECK(f.document->asset()==edited);
    CHECK(source.beginEdit("Group gesture"));
    std::vector<std::string> ids={"0","1"};
    CHECK_FALSE(source.transformKeys(ids,0.9,0,1));
    CHECK(source.transformKeys(ids,0.1,0,0.1f));
    CHECK(source.endEdit(false));
    CHECK(f.document->asset().emitters[1].effect.sizeCurve.front().time==0);
    CHECK(f.document->undo()); CHECK(f.document->asset()==edited);
    CHECK(source.beginEdit("Stale gesture")); CHECK(source.updateKey(key,0.5,{6}));
    auto external=edited; external.name="External edit"; CHECK(f.document->replaceAsset(external));
    CHECK(source.endEdit(false)); CHECK(f.document->asset()==external);
}
TEST_CASE(curve_ui_add_value_gradient_save_and_invalid_time) {
    Fixture f; Host host; host.root=f.root.string();
    EditorExtensionRegistry registry; CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);
    auto* root=view->rootWidget();
    const auto original=f.document->asset();
    auto* initial=dynamic_cast<ayt::ui::TextInput*>(find(root,"particle_key_value_0"));
    CHECK_NOT_NULL(initial); initial->setFocus(true); initial->onKeyDown(ayt::ui::UIKey_Enter);
    CHECK(f.document->asset()==original); CHECK_FALSE(f.document->isDirty());
    click(root,"particle_add_key"); view->tick(0);
    CHECK(f.document->asset().emitters[0].effect.sizeCurve.size()==4);
    auto* value=dynamic_cast<ayt::ui::TextInput*>(find(root,"particle_key_value_0")); CHECK_NOT_NULL(value);
    value->setFocus(true); value->setText(L"2.5"); value->onKeyDown(ayt::ui::UIKey_Enter); view->tick(0);
    CHECK(f.document->asset().emitters[0].effect.sizeCurve[2].value==2.5f);
    const auto beforeInvalid=f.document->asset();
    auto* time=dynamic_cast<ayt::ui::TextInput*>(find(root,"particle_key_time")); CHECK_NOT_NULL(time);
    time->setFocus(true); time->setText(L"0"); time->onKeyDown(ayt::ui::UIKey_Enter); view->tick(0);
    CHECK(f.document->asset()==beforeInvalid);
    click(root,"command_edit.undo"); view->tick(0);
    click(root,"command_edit.undo"); view->tick(0); CHECK(f.document->asset()==original);
    auto* track=dynamic_cast<ayt::ui::ComboBox*>(find(root,"particle_lifetime_track")); CHECK_NOT_NULL(track);
    track->setSelectedIndexAndNotify(2); view->tick(0);
    CHECK_NOT_NULL(find(root,"particle_gradient_strip"));
    click(root,"particle_add_key"); view->tick(0);
    CHECK(f.document->asset().emitters[0].effect.colorGradient.size()==4);
    auto* gradient=dynamic_cast<ayt::ui::ColorPicker*>(find(root,"particle_gradient_key_color")); CHECK_NOT_NULL(gradient);
    gradient->sampleColor({.25f,.5f,.75f,.9f}); view->tick(0);
    CHECK(std::abs(f.document->asset().emitters[0].effect.colorGradient[2].color.r-.25f)<.001f);
    click(root,"command_file.save"); view->tick(0);
    EditorParticleDocument reopened; std::string error;
    CHECK(reopened.initialize({f.document->path()},error)); CHECK(reopened.asset()==f.document->asset());
}
TEST_CASE(curve_pointer_gesture_undo_and_capture_cancel) {
    Fixture f; auto asset=f.document->asset();
    asset.emitters[0].effect.sizeCurve={{0,1},{0.5f,2},{1,1}};
    CHECK(f.document->replaceAsset(asset));
    Host host; host.root=f.root.string(); EditorExtensionRegistry registry;
    CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);
    auto* canvas=dynamic_cast<ayt::ui::authoring::CurveCanvas*>(find(view->rootWidget(),"particle_lifetime_canvas")); CHECK_NOT_NULL(canvas);
    canvas->setPosition({0,0}); canvas->setSize({320,170});
    const ayt::math::FVector2 point{48+260*0.5f,18+132*(2.15f-2)/1.3f};
    CHECK(canvas->onMouseButtonDown({point,0}));
    CHECK(canvas->onMouseMove({{point.x+10,point.y+20},0}));
    view->tick(0); CHECK(f.document->asset()==asset);
    CHECK(canvas->onMouseButtonUp({{point.x+10,point.y+20},0}));
    CHECK(f.document->asset()!=asset);
    CHECK(f.document->undo()); CHECK(f.document->asset()==asset);
    view->tick(0);
    canvas=dynamic_cast<ayt::ui::authoring::CurveCanvas*>(find(view->rootWidget(),"particle_lifetime_canvas"));
    canvas->setPosition({0,0}); canvas->setSize({320,170});
    CHECK(canvas->onMouseButtonDown({point,0})); CHECK(canvas->onMouseMove({{point.x,point.y+10},0}));
    canvas->onCaptureCancelled(); CHECK(f.document->asset()==asset);
    view->prepareForUiShutdown();
}

TEST_CASE(typed_sources_probability_animation_and_history_ui) {
    Fixture f;Host host;host.root=f.root.string();EditorExtensionRegistry registry;CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);auto* root=view->rootWidget();
    const auto select=[&](const char* id,int n){auto* box=dynamic_cast<ayt::ui::ComboBox*>(find(root,id));CHECK_NOT_NULL(box);box->setSelectedIndexAndNotify(n);view->tick(0);};
    const auto submit=[&](const char* id,const wchar_t* text){auto* box=dynamic_cast<ayt::ui::TextInput*>(find(root,id));CHECK_NOT_NULL(box);box->setFocus(true);box->setText(text);box->onKeyDown(ayt::ui::UIKey_Enter);view->tick(0);};
    select("particle_value_attribute",int(ayt::particle::FloatAttribute::AnimationFps));select("particle_value_mode",2);
    submit("particle_value_max",L"12");submit("particle_value_min",L"5");
    const auto& fps=f.document->asset().emitters[0].effect.value(ayt::particle::FloatAttribute::AnimationFps);
    CHECK(fps.min==5&&fps.max==12);select("particle_flip_x_mode",1);submit("particle_flip_x_probability",L"0.75");
    select("particle_animation_playback",2);CHECK(f.document->asset().emitters[0].effect.animationPlayback==ayt::particle::AnimationPlayback::PingPong);
    const auto beforeBad=f.document->asset();submit("particle_flip_x_probability",L"2");CHECK(f.document->asset()==beforeBad);
    select("particle_value_attribute",int(ayt::particle::FloatAttribute::Opacity));select("particle_value_mode",5);
    CHECK_NOT_NULL(find(root,"particle_source_canvas"));click(root,"particle_source_add_key");view->tick(0);submit("particle_source_value",L"0.5");
    CHECK(f.document->asset().emitters[0].effect.value(ayt::particle::FloatAttribute::Opacity).curve.size()==3);
    select("particle_source_curve_select",1);click(root,"particle_source_add_key");view->tick(0);submit("particle_source_value",L"0.8");
    const auto final=f.document->asset();click(root,"command_file.save");view->tick(0);
    EditorParticleDocument loaded;std::string error;CHECK(loaded.initialize({f.document->path()},error));CHECK(loaded.asset()==final);
    CHECK(f.document->undo());CHECK(f.document->redo());CHECK(f.document->asset()==final);
    click(root,"particle_add_sprite");view->tick(0);CHECK(f.document->asset().emitters.back().kind==ayt::particle::EffectLayerKind::SpriteAnimation);
}
TEST_CASE(preview_seed_seek_views_leave_authoring_unchanged) {
    Fixture f;Host host;host.root=f.root.string();EditorExtensionRegistry registry;CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);auto* root=view->rootWidget();const auto original=f.document->asset();
    click(root,"particle_view_3d");auto* preview=find(root,"particle_preview");CHECK(preview->onMouseButtonDown({{30,40},1}));CHECK(preview->onMouseMove({{90,60},1}));CHECK(preview->onMouseButtonUp({{90,60},1}));
    click(root,"particle_view_2d");click(root,"particle_preview_reroll");CHECK(f.document->previewSeed()!=original.seed);
    CHECK(f.document->seek(.5f));CHECK(std::abs(f.document->preview()->time()-.5)<.001);const auto p=f.document->preview()->instances()[1].particles()[0];
    CHECK(f.document->seek(.5f));CHECK(f.document->preview()->instances()[1].particles()[0].position==p.position);
    CHECK_FALSE(f.document->seek(61));CHECK(f.document->asset()==original);CHECK_FALSE(f.document->isDirty());CHECK_NOT_NULL(find(root,"particle_activity"));
}
TEST_CASE(layer_duplicate_reorder_and_save_keep_stable_ids) {
    Fixture f;Host host;host.root=f.root.string();EditorExtensionRegistry registry;CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);auto* root=view->rootWidget();
    const auto original=f.document->asset();
    click(root,"particle_layer_duplicate");view->tick(0);
    CHECK(f.document->asset().emitters.size()==original.emitters.size()+1);
    const auto id=f.document->asset().emitters[1].id;
    CHECK(id!=0);CHECK(f.document->asset().emitters[1].name!=original.emitters[0].name);
    click(root,"particle_layer_down");view->tick(0);
    CHECK(f.document->asset().emitters[2].id==id);
    click(root,"particle_layer_up");view->tick(0);
    CHECK(f.document->asset().emitters[1].id==id);
    const auto saved=f.document->asset();CHECK(f.document->saveAs((f.root/"organized.ayparticle").string()));
    EditorParticleDocument loaded;std::string error;CHECK(loaded.initialize({f.document->path()},error));CHECK(loaded.asset()==saved);
    CHECK(f.document->undo());CHECK(f.document->redo());CHECK(f.document->asset()==saved);
}
TEST_CASE(two_dimensional_canvas_moves_and_resizes_in_one_undo_step) {
    Fixture f;auto asset=f.document->asset();asset.emitters.resize(1);
    auto& effect=asset.emitters[0].effect;effect.dimension=ayt::particle::Dimension::TwoD;
    effect.shape=ayt::particle::Shape::Rectangle;effect.extent={.5f,.5f,0};
    CHECK(f.document->replaceAsset(asset));
    Host host;host.root=f.root.string();EditorExtensionRegistry registry;CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);auto* canvas=find(view->rootWidget(),"particle_preview");CHECK_NOT_NULL(canvas);
    canvas->setPosition({0,0});canvas->setSize({400,400});
    const auto before=f.document->asset();const auto originalRevision=f.document->revision();
    CHECK(canvas->onMouseButtonDown({{200,200},0}));CHECK(canvas->onMouseMove({{257,143},0}));
    CHECK(f.document->asset()==before);CHECK(canvas->onMouseButtonUp({{257,143},0}));
    CHECK(f.document->revision()==originalRevision+1);
    CHECK(std::abs(f.document->asset().emitters[0].offset.x-1.f)<.01f);
    CHECK(std::abs(f.document->asset().emitters[0].offset.y-1.f)<.01f);
    CHECK(f.document->undo());CHECK(f.document->asset()==before);CHECK(f.document->redo());
    view->tick(0);
    const auto moved=f.document->asset();const auto revision=f.document->revision();
    const float scale=400.f/f.document->previewSettings().screenHeight;
    const ayt::math::FVector2 handle{257+effect.extent.x*scale,143};
    CHECK(canvas->onMouseButtonDown({handle,0}));CHECK(canvas->onMouseMove({{handle.x+scale,handle.y},0}));
    CHECK(canvas->onMouseButtonUp({{handle.x+scale,handle.y},0}));
    CHECK(f.document->revision()==revision+1);
    CHECK(std::abs(f.document->asset().emitters[0].effect.extent.x-1.5f)<.01f);
    CHECK(f.document->undo());CHECK(f.document->asset()==moved);
    const ayt::math::FVector2 yHandle{257,143-effect.extent.y*scale};
    CHECK(canvas->onMouseButtonDown({yHandle,0}));
    CHECK(canvas->onMouseMove({{yHandle.x,yHandle.y-scale},0}));
    CHECK(canvas->onMouseButtonUp({{yHandle.x,yHandle.y-scale},0}));
    CHECK(std::abs(f.document->asset().emitters[0].effect.extent.y-1.5f)<.01f);
    CHECK(f.document->undo());CHECK(f.document->asset()==moved);
    CHECK(canvas->onMouseButtonDown({{257,143},0}));CHECK(canvas->onMouseMove({{300,150},0}));
    canvas->onCaptureCancelled();CHECK(f.document->asset()==moved);
}
TEST_CASE(overlapping_canvas_handles_prefer_selected_layer) {
    Fixture f;auto asset=f.document->asset();asset.emitters.resize(2);
    for(auto& layer:asset.emitters){layer.offset={};layer.effect.dimension=ayt::particle::Dimension::TwoD;layer.effect.shape=ayt::particle::Shape::Point;}
    CHECK(f.document->replaceAsset(asset));
    Host host;host.root=f.root.string();EditorExtensionRegistry registry;CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);
    auto* canvas=find(view->rootWidget(),"particle_preview");CHECK_NOT_NULL(canvas);
    canvas->setPosition({0,0});canvas->setSize({400,400});
    CHECK(canvas->onMouseButtonDown({{200,200},0}));
    CHECK(canvas->onMouseButtonUp({{230,200},0}));
    CHECK(f.document->asset().emitters[0].offset.x>0);
    CHECK(f.document->asset().emitters[1].offset.x==0);
}
TEST_CASE(visual_color_and_packaged_composite_example) {
    const auto effectPath=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        /"AYParticle/examples/FallingLeaves/Assets/effects/AutumnGust.ayparticle";
    EditorParticleDocument example;std::string error;CHECK(example.initialize({effectPath.string()},error));
    CHECK(example.asset().emitters.size()==3);
    CHECK(example.asset().emitters[0].effect.texturePath=="textures/leaf-flipbook.png");
    CHECK(example.asset().emitters[1].effect.texturePath.empty());
    CHECK(example.asset().emitters[2].kind==ayt::particle::EffectLayerKind::SpriteAnimation);
    example.play();example.tick(.5f);CHECK(example.preview()->liveParticles()>0);
    Fixture f;
    const auto textures=f.root/"Assets/textures";std::filesystem::create_directories(textures);
    std::filesystem::copy_file(effectPath.parent_path().parent_path()/"textures/leaf-flipbook.png",
                               textures/"leaf-flipbook.png");
    Host host;host.root=f.root.string();EditorExtensionRegistry registry;CHECK(registerEditorParticleExtension(registry));
    auto view=registry.find(kEditorParticleExtensionId)->createView(f.document,host);
    auto* root=view->rootWidget();CHECK_NOT_NULL(find(root,"particle_texture_browse"));
    CHECK_NOT_NULL(find(root,"particle_texture_preview"));
    click(root,"particle_texture_gallery_toggle");view->tick(0);
    auto* tile=find(root,"particle_texture_tile_textures/leaf-flipbook.png");CHECK_NOT_NULL(tile);
    tile->setPosition({0,0});tile->setSize({140,90});
    CHECK(tile->onMouseButtonDown({{20,20},0}));CHECK(tile->onMouseButtonUp({{20,20},0}));view->tick(0);
    CHECK(f.document->asset().emitters[0].effect.texturePath=="textures/leaf-flipbook.png");
    auto* picker=dynamic_cast<ayt::ui::ColorPicker*>(find(root,"particle_color_picker"));CHECK_NOT_NULL(picker);
    picker->sampleColor({.2f,.4f,.6f,.8f});view->tick(0);
    const auto& color=f.document->asset().emitters[0].effect.startColor;
    CHECK(std::abs(color.r-.2f)<.001f);CHECK(std::abs(color.a-.8f)<.001f);
    CHECK(f.document->undo());
    view->tick(0);
    auto* track=dynamic_cast<ayt::ui::ComboBox*>(find(root,"particle_lifetime_track"));CHECK_NOT_NULL(track);
    track->setSelectedIndexAndNotify(2);view->tick(0);
    if(!find(root,"particle_gradient_key_color")) {click(root,"particle_toggle_curve");view->tick(0);}
    auto* gradient=dynamic_cast<ayt::ui::ColorPicker*>(find(root,"particle_gradient_key_color"));CHECK_NOT_NULL(gradient);
    gradient->sampleColor({.8f,.3f,.1f,.9f});view->tick(0);
    CHECK(std::abs(f.document->asset().emitters[0].effect.colorGradient[0].color.g-.3f)<.001f);
}
TEST_SUITE_END
#endif
