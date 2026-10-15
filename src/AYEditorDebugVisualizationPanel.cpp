#include <AYEditor/EditorSession.h>
#include "EditorDebugVisualization.h"
#include <AYPhysics/PhysicsSubSystem.h>
#include <AYPhysics/PhysicsManager.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYUI/CheckBox.h>
#include <AYUI/TextLabel.h>
#include <AYUI/TextInput.h>
#include <AYUI/ComboBox.h>
#include <AYUI/Button.h>
#include <AYUI/Box.h>
#include <AYEntity.h>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <AYUI/ScrollView.h>
namespace ayt::editor {
bool EditorSession::setDebugVisualization(const EditorDebugVisualizationOptions& options) {
    if((options.categories&~0x1ffu) || options.trailFrames<2 || options.trailFrames>600) return false;
    _debugVisualization->options=options;
    _physicsDebugColliders=(options.categories&1)!=0;_physicsDebugQueries=(options.categories&2)!=0;
    if(auto* physics=ayt::physics::PhysicsSubSystem::findRegistered()) if(auto* manager=physics->manager()) {
        (void)manager->setDebugSnapshotEnabled((options.categories&0x13fu)!=0,options.categories);
        manager->debugDraw().setFrozen(options.frozen);
    }
    return true;
}
EditorDebugVisualizationOptions EditorSession::debugVisualizationOptions() const {return _debugVisualization->options;}
void EditorSession::stepDebugVisualization() {
    _debugVisualization->stepRequested=true;
    if(auto* physics=ayt::physics::PhysicsSubSystem::findRegistered()) if(auto* manager=physics->manager()) manager->debugDraw().setFrozen(false);
}
void EditorSession::clearDebugVisualization(int category) {
    _debugVisualization->clear(category);_debugVisualizationDetail.clear();
    if(auto* physics=ayt::physics::PhysicsSubSystem::findRegistered()) if(auto* manager=physics->manager()) {
        if(category<0) manager->debugDraw().clear();
        else if(category<=8) manager->debugDraw().clearCategory(static_cast<ayt::physics::PhysicsDebugCategory>(category));
    }
}
void EditorSession::showDebugVisualizationPanel() {
    if(_debugVisualizationPanel) {
        _debugVisualizationPanel->setVisible(true);
        for(uint32_t i=0;i<9;++i) if(auto* c=dynamic_cast<ayt::ui::CheckBox*>(_ui.findById("debug_category_"+std::to_string(i)))) c->setChecked((_debugVisualization->options.categories&(1u<<i))!=0);
        return;
    }
    if(!_ui.root()) return;
    auto* panel=new ayt::ui::DockCard();_debugVisualizationPanel=panel;
    panel->setId("debug_visualization_panel");static_cast<ayt::ui::Widget*>(panel)->setId("debug_visualization_panel");panel->setTitle(L"Debug Visualization");panel->setClosable(true);
    panel->setOnCloseRequested([](auto* card){card->setVisible(false);});
    panel->setPosition({28,70});panel->setSize({420,std::clamp(_ui.root()->getSize().y-90.f,300.f,680.f)});
    auto* body=new ayt::ui::VBox();body->setSpacing(4);
    const wchar_t* names[]={L"Collision shapes",L"Queries / hit points",L"Contacts / normals / impulse",L"Character controller",L"Velocity / motion trails",L"Joints / anchors / limits",L"Camera frusta / clipping",L"Skeleton / joint axes",L"Ability regions / affected targets"};
    for(uint32_t i=0;i<9;++i) {
        auto* check=new ayt::ui::CheckBox();check->setId("debug_category_"+std::to_string(i));check->setText(names[i]);
        check->setChecked((_debugVisualization->options.categories&(1u<<i))!=0);
        check->setOnToggled([this,i](bool enabled){auto o=debugVisualizationOptions();if(enabled)o.categories|=1u<<i;else o.categories&=~(1u<<i);(void)setDebugVisualization(o);});body->addWidget(check,24);
    }
    auto toggle=[&](const wchar_t* text,bool value,auto action) {auto* c=new ayt::ui::CheckBox();c->setText(text);c->setChecked(value);c->setOnToggled(action);body->addWidget(c,24);};
    toggle(L"Selected entity only",_debugVisualization->options.selectedOnly,[this](bool value){auto o=debugVisualizationOptions();o.selectedOnly=value;(void)setDebugVisualization(o);});
    toggle(L"Occlude behind scene geometry",_debugVisualization->options.depthTest,[this](bool value){auto o=debugVisualizationOptions();o.depthTest=value;(void)setDebugVisualization(o);});
    toggle(L"Freeze captured records",_debugVisualization->options.frozen,[this](bool value){auto o=debugVisualizationOptions();o.frozen=value;(void)setDebugVisualization(o);});
    auto labeled=[&](const wchar_t* text,ayt::ui::Widget* field) {auto* row=new ayt::ui::HBox();auto* label=new ayt::ui::TextLabel();label->setText(text);row->addWidget(label,160);row->addWidget(field);body->addWidget(row,26);};
    std::wostringstream layerText;layerText<<std::hex<<_debugVisualization->options.layerMask;
    auto* mask=new ayt::ui::TextInput();mask->setText(layerText.str());mask->setAccessibilityDescription(L"Physical layer mask (hexadecimal)");
    mask->setOnTextChanged([this](const auto& text){try {size_t n=0;auto value=std::stoull(text,&n,16);if(n==text.size() && value<=0xffffffffu){auto o=debugVisualizationOptions();o.layerMask=uint32_t(value);(void)setDebugVisualization(o);}}catch(...) {}});labeled(L"Layers (hex mask)",mask);
    auto* lifetime=new ayt::ui::ComboBox();lifetime->setItems({L"Real-time",L"Timed",L"Keep until cleared"});lifetime->setSelectedIndex(_physicsDebugLifetime);
    lifetime->setOnSelectionChanged([this](int index){if(index>=0 && index<=2) { _physicsDebugLifetime=uint8_t(index); }});body->addWidget(lifetime,26);
    auto* duration=new ayt::ui::TextInput();duration->setText(std::to_wstring(_physicsDebugSeconds));duration->setAccessibilityDescription(L"History duration in seconds (0.001 to 86400)");
    duration->setOnTextChanged([this](const auto& text){try {size_t n=0;const auto value=std::stod(text,&n);if(n==text.size() && std::isfinite(value) && value>=.001 && value<=86400)_physicsDebugSeconds=value;}catch(...) {}});labeled(L"Duration (seconds)",duration);
    auto* clearCategory=new ayt::ui::ComboBox();clearCategory->setItems({L"Clear all categories",names[0],names[1],names[2],names[3],names[4],names[5],names[6],names[7],names[8]});clearCategory->setSelectedIndex(0);body->addWidget(clearCategory,26);
    auto button=[&](const wchar_t* text,auto action){auto* b=new ayt::ui::Button();b->setText(text);b->setOnClicked(action);body->addWidget(b,26);};
    button(L"Clear selected category",[this,clearCategory]{clearDebugVisualization(clearCategory->getSelectedIndex()-1);});
    button(L"Capture next frame",[this]{stepDebugVisualization();});
    _debugVisualizationStats=new ayt::ui::TextLabel();_debugVisualizationStats->setId("debug_visualization_statistics");_debugVisualizationStats->setWordWrap(true);_debugVisualizationStats->setWrapWidth(380);body->addWidget(_debugVisualizationStats,200);
    auto* scroll=new ayt::ui::ScrollView();scroll->setId("debug_visualization_scroll");scroll->setContentOwned(body);scroll->setContentSize({396,760});
    scroll->setHorizontalScrollBarEnabled(false);panel->setContent(scroll);_ui.root()->addChild(panel);
}
void EditorSession::syncPhysicsDebugToRenderer() {
    const auto* world=hierarchyWorld();if(_physicsDebugWorld!=world) clearPhysicsDebugDraw();
    auto* subsystem=ayt::render::RendererSubSystem::findRegistered();if(!subsystem) return;
    auto& model=*_debugVisualization;
    std::vector<ayt::physics::PhysicsDebugRecord> records;
    uint64_t frame=0;float seconds=0;size_t dropped=0;bool native=false;
    if(auto* physics=ayt::physics::PhysicsSubSystem::findRegistered()) if(auto* manager=physics->manager()) {
        (void)manager->setDebugSnapshotEnabled((model.options.categories&0x13fu)!=0,model.options.categories);
        auto options=manager->debugDraw().options();options.enabled=(model.options.categories&((1u<<1)|(1u<<2)|(1u<<8)))!=0;
        options.lifetime=static_cast<ayt::physics::DebugDrawLifetime>(_physicsDebugLifetime);
        options.duration=ayt::time::Duration::fromUs(int64_t(_physicsDebugSeconds*1000000));(void)manager->debugDraw().configure(options);
        manager->debugDraw().setFrozen(model.options.frozen && !model.stepRequested);
        auto snapshot=manager->fetchDebugSnapshot();frame=snapshot.frameIndex;seconds=snapshot.stepSeconds;dropped=snapshot.droppedRecords;
        native=frame!=0;
        for(auto& r:snapshot.records) if(r.category==ayt::physics::PhysicsDebugCategory::Contact) (void)manager->debugDraw().recordGeometry(std::move(r));else records.push_back(std::move(r));
        for(auto& r:manager->debugDraw().consumeRecords()) records.push_back(std::move(r));
        dropped+=manager->debugDraw().droppedRecords();
    }
    if(world) {
        ayt::math::FRectangle bounds;const float aspect=getViewportBounds(bounds)&&bounds.height()>0?bounds.width()/bounds.height():1.f;
        for(auto& r:editorSceneDebugRecords(*world,aspect,model.options.categories,native)) records.push_back(std::move(r));
        resolveDebugEntities(records,*world);
    }
    if(!frame) frame=model.frameIndex+1; // Edit mode still has diagnostic presentation frames.
    model.update(std::move(records),frame,seconds);
    std::vector<ayt::render::EditorDebugLine> lines;
    for(const auto& r:model.records) if(model.visible(r,_selection.entityId())) for(const auto& l:r.lines) lines.push_back({l.from,l.to,l.color});
    subsystem->renderer().setEditorDebugDepthTest(model.options.depthTest);(void)subsystem->renderer().setEditorDebugLines(lines);
    if(_debugVisualizationStats) {
        _debugVisualizationStats->setText(L"Frame "+std::to_wstring(model.frameIndex)+L" | Records "+std::to_wstring(model.records.size())+L" / 1024\nLines "+std::to_wstring(lines.size())+L" / 16384 | Dropped "+std::to_wstring(model.droppedRecords+dropped)+L"\n"+_debugVisualizationDetail);
    }
}
}
