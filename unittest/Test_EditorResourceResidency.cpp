#include <AYTest.h>
#include <AYRenderer.h>
#include <AYRenderer/ResourceResidency.h>
#include "../src/EditorDebugVisualization.h"
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYScene.h>
#include <AYApplication/IEngineHost.h>
#include <AYResource/ResourceManager.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/ScrollView.h>
#include "fixtures/EditorShellLayout.h"
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace ayt;
namespace {
constexpr uint32_t resources=1u<<uint32_t(physics::PhysicsDebugCategory::Resource);
}
TEST_SUITE(AYEditor_ResourceResidency)
TEST_CASE(disabled_category_returns_no_rows_and_uninitialized_renderer_is_explicit)
{
    render::Renderer renderer;
    CHECK(editor::editorRendererResourceDebugRecords(renderer,0).empty());
    const auto unavailable=editor::editorRendererResourceDebugRecords(renderer,resources);
    CHECK(unavailable.size()==1);
    if(unavailable.size()==1)CHECK(unavailable[0].details.find("unavailable before initialization")!=std::string::npos);
    CHECK_FALSE(renderer.isInitialized());
}
TEST_CASE(diagnostic_rows_explain_logical_storage_source_payload_and_read_only_ownership)
{
    render::Renderer renderer;render::InitDesc init;init.backend=render::Backend::Noop;
    CHECK(renderer.initialize(init));const uint8_t pixels[16]={};
    auto texture=renderer.createTextureFromRgba8(2,2,pixels,"editor observed");CHECK(texture.isValid());
    const auto before=renderer.resourceResidencySnapshot();
    const auto rows=editor::editorRendererResourceDebugRecords(renderer,resources);CHECK(rows.size()==2);
    if(rows.size()==2){
        CHECK(rows[0].label=="Renderer texture residency");
        CHECK(rows[0].details.find("Known owned logical texture bytes 16")!=std::string::npos);
        CHECK(rows[0].details.find("not cumulative bandwidth")!=std::string::npos);
        CHECK(rows[1].details.find("format RGBA8 | levels 1")!=std::string::npos);
        CHECK(rows[1].details.find("Provided pixel/block payload RGBA8 | 1 levels | 16 bytes")!=std::string::npos);
        CHECK(rows[1].details.find("do not retain GPU resources")!=std::string::npos);
    }
    const auto after=renderer.resourceResidencySnapshot();
    CHECK(before.textureEntries==after.textureEntries);CHECK(before.knownOwnedLogicalTextureBytes==after.knownOwnedLogicalTextureBytes);
    CHECK(editor::editorRendererResourceDebugRecords(renderer,0).empty());
    renderer.destroyTexture(texture);
}
TEST_CASE(renderer_rows_reserve_a_bounded_share_of_the_existing_resource_category)
{
    render::Renderer renderer;render::InitDesc init;init.backend=render::Backend::Noop;
    CHECK(renderer.initialize(init));const uint8_t pixel[4]={};
    for(unsigned index=0;index<30;++index)CHECK(renderer.createTextureFromRgba8(1,1,pixel,"debug-row-"+std::to_string(index)).isValid());
    const auto rows=editor::editorRendererResourceDebugRecords(renderer,resources);
    CHECK(rows.size()==25);if(!rows.empty())CHECK(rows[0].details.find("omitted texture rows 6")!=std::string::npos);
    for(const auto& row:rows){CHECK(row.category==physics::PhysicsDebugCategory::Resource);CHECK(row.lines.empty());}
}
TEST_CASE(diagnostic_controls_have_readable_layout_and_details_are_scrollable)
{
    ui::MockRenderer backend;editor::EditorSession session;
    CHECK(session.initialize(&backend,editor::testfixtures::resolveEditorShellLayoutPath()));
    session.setClientSize(1280,800);session.showDebugVisualizationPanel();
    auto* panel=session.ui().findById("debug_visualization_panel");CHECK(panel);if(!panel)return;
    session.ui().layout();
    auto* first=session.ui().findById("debug_category_0");
    auto* second=session.ui().findById("debug_category_1");
    auto* browser=session.ui().findById("debug_record_browser");
    auto* text=session.ui().findById("debug_visualization_statistics");
    auto* scroll=dynamic_cast<ui::ScrollView*>(session.ui().findById("debug_visualization_scroll"));
    CHECK(first);CHECK(second);CHECK(browser);CHECK(text);CHECK(scroll);
    if(first&&second&&browser&&text&&scroll){
        CHECK(first->getSize().x>=300);CHECK(first->getSize().y>=20);
        CHECK(second->getWorldBounds().minY>=first->getWorldBounds().maxY);
        CHECK(browser->getSize().x>=300);CHECK(text->getSize().x>=300);CHECK(text->getSize().y>=400);
        scroll->setScrollOffset({0,100000});session.ui().layout();
        CHECK(scroll->getScrollOffset().y>0);
        CHECK(text->getWorldBounds().minY<scroll->getClientRect().maxY);
        CHECK(text->getWorldBounds().maxY>scroll->getClientRect().minY);
    }
    session.shutdown();
}
TEST_CASE(resource_diagnostics_stop_after_explicit_disable_and_session_shutdown)
{
    app::EngineHostScope hostScope{app::defaultEngineHost()};
    auto& manager=resource::ResourceManager::instance();
    manager.setDebugCaptureEnabled(false);
    const auto watched=std::filesystem::temp_directory_path()/
        ("ay-residency-diagnostic-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp");
    struct Reset{
        resource::ResourceManager& manager;std::filesystem::path path;
        ~Reset(){manager.unwatchResource(path.string());manager.unloadResource(path.string());
            manager.setDebugCaptureEnabled(false);manager.setHotReloadDebounceSeconds(.1f);
            manager.setHotReloadPollIntervalSeconds(.25f);std::error_code error;std::filesystem::remove(path,error);}
    } reset{manager,watched};
    {std::ofstream file(watched);file<<"initial";CHECK(bool(file));}
    auto stamp=std::filesystem::last_write_time(watched);
    manager.setHotReloadPollIntervalSeconds(0);manager.setHotReloadDebounceSeconds(0);
    manager.watchResource(watched.string());
    ui::MockRenderer backend;editor::EditorSession session;
    CHECK(session.initialize(&backend,editor::testfixtures::resolveEditorShellLayoutPath()));
    // Establish the current debug World so shutdown cannot accidentally pass
    // by taking the different-World cleanup path.
    CHECK(session.setPhysicsDebugDraw(false,false));
    editor::EditorDebugVisualizationOptions options;options.categories=resources;
    CHECK(session.setDebugVisualization(options));
    auto* document=session.document();CHECK(document);if(!document)return;
    const auto collect=[&]{(void)editor::editorServiceDebugRecords(document->scene().world(),session.ui(),resources);};
    const auto observe=[&](const char* bytes){
        {std::ofstream file(watched);file<<bytes;CHECK(bool(file));}
        stamp+=std::chrono::seconds(2);std::filesystem::last_write_time(watched,stamp);
        manager.update(0); // Actual watch notifications journal; author refresh reports do not.
    };
    collect();observe("before");
    CHECK(!manager.debugSnapshot().reloads.empty());
    CHECK(session.setDebugVisualization({}));
    observe("disabled");CHECK(manager.debugSnapshot().reloads.empty());
    CHECK(session.setDebugVisualization(options));collect();
    observe("reopened");CHECK(!manager.debugSnapshot().reloads.empty());
    session.shutdown();
    observe("after-shutdown");
    CHECK(manager.debugSnapshot().reloads.empty());
}
TEST_SUITE_END
