#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/World.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/AssetPath.h>
#include <AYResource/assetsDefs/IMesh.h>
#include <AYScene.h>
#include <AYUI/Authoring/PropertyField.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Box.h>
#include <AYUI/CheckBox.h>
#include <AYUI/MenuItem.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/TextLabel.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace ayt::editor {
bool EditorSession::setTransformSnap(const EditorTransformSnap& settings)
{
    if(!settings.valid())return false;
    if(_transformGizmo.active())finishTransformGizmoDrag(false);
    _preferences.transformSnap=settings;
    auto* widget=_ui.findById("menu_transform_snap");
    if(!widget){
        const auto find=[](auto&& self,ui::Widget* root)->ui::Widget* {
            if(!root)return nullptr;if(root->getId()=="menu_transform_snap")return root;
            for(auto* child:root->getChildren())if(auto* found=self(self,child))return found;
            return nullptr;
        };
        widget=find(find,_ui.getOverlayRoot());
    }
    if(auto* item=dynamic_cast<ui::MenuItem*>(widget)) {
        item->setLocalizationKey("text",settings.enabled ? "ui.editor.viewport.snap_on" : "ui.editor.viewport.snap_off");
        item->setText(localizedText(settings.enabled ? "ui.editor.viewport.snap_on" : "ui.editor.viewport.snap_off",
            settings.enabled ? "Transform Snapping: On" : "Transform Snapping: Off"));
    }
    if(_repaintCallback)_repaintCallback();
    return true;
}

void EditorSession::showTransformSnapDialog()
{
    _transformSnapDialog=std::make_unique<ui::ModalDialog>();
    auto* dialog=_transformSnapDialog.get();
    dialog->setId("transform_snap_dialog");dialog->setSize({600,310});
    dialog->setAcceptText(localizedText("ui.editor.import.apply","Apply"));
    dialog->setRejectText(localizedText("ui.editor.import.cancel","Cancel"));
    auto* body=new ui::VBox();body->setSize({568,238});body->setSpacing(5);
    auto* enabled=new ui::CheckBox();enabled->setId("transform_snap_enabled");
    enabled->setText(localizedText("ui.editor.viewport.snap_enabled","Enable transform snapping"));
    enabled->setChecked(transformSnap().enabled);body->addWidget(enabled,28);
    const char* ids[]={"translation","rotation","scale"};
    const char* keys[]={"ui.editor.viewport.snap_translation","ui.editor.viewport.snap_rotation","ui.editor.viewport.snap_scale"};
    const char* labels[]={"Move increment (meters)","Rotate increment (degrees)","Scale ratio increment"};
    const float values[]={transformSnap().translation,transformSnap().rotationDegrees,transformSnap().scale};
    std::array<ui::authoring::PropertyField*,3> fields{};
    for(std::size_t i=0;i<fields.size();++i){
        ui::authoring::PropertyFieldOptions options;
        options.inputId=std::string("transform_snap_")+ids[i];options.labelWidth=260;
        fields[i]=new ui::authoring::PropertyField(options);
        fields[i]->setTextValue(localizedText(keys[i],labels[i]),ui::authoring::formatNumber(values[i],9));
        body->addWidget(fields[i],28);
    }
    auto* hint=new ui::TextLabel();hint->setWordWrap(true);
    hint->setText(localizedText("ui.editor.viewport.snap_hint",
        "3D drag increments start at the original pose. Hold Ctrl to invert snapping. Scene data is unchanged until the drag is committed."));
    body->addWidget(hint,44);
    auto* error=new ui::TextLabel();error->setId("transform_snap_error");
    error->setWordWrap(true);error->setTextColor({1,.4f,.3f,1});body->addWidget(error,40);
    dialog->setBodyContentOwned(body);
    dialog->setAcceptValidator([this,enabled,fields,error]{
        EditorTransformSnap settings;settings.enabled=enabled->isChecked();
        const auto move=ui::authoring::parseFiniteFloat(fields[0]->value());
        const auto rotate=ui::authoring::parseFiniteFloat(fields[1]->value());
        const auto scale=ui::authoring::parseFiniteFloat(fields[2]->value());
        if(move && rotate && scale){settings.translation=*move;settings.rotationDegrees=*rotate;settings.scale=*scale;}
        if(!move || !rotate || !scale || !setTransformSnap(settings)){
            error->setText(localizedText("ui.editor.viewport.snap_invalid",
                "Use Move 0.00001..10000, Rotate 0.01..180, and Scale 0.001..10."));return false;
        }
        return true;
    });
    dialog->fitAndCenterInViewport(_ui.root()->getSize(),12);dialog->openModal();
    if(_repaintCallback)_repaintCallback();
}

bool EditorSession::frameSelectedEntity()
{
    if(_gameView.mode()!=EditorMode::Edit || !_document || _sceneCamera.isTwoD()
        || _transformGizmo.active() || _sceneCamera.threeD().isLooking()
        || _selectionWorld!=&_document->scene().world())return false;
    auto* selected=_selection.resolve(_selectionWorld);
    if(!selected || !selected->transform())return false;
    const auto finite=[](const math::FVector3& v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
    math::FVector3 low{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    math::FVector3 high{-low.x,-low.y,-low.z};
    std::vector<entity::Entity*> pending{selected};std::unordered_set<entity::Entity*> visited;
    while(!pending.empty()){
        auto* object=pending.back();pending.pop_back();
        if(!object || !visited.insert(object).second)continue;
        for(auto* child:object->spatialChildren())pending.push_back(child);
        if(!object->transform())continue;
        math::FVector3 a{-.5f,-.5f,-.5f},b{.5f,.5f,.5f};
        if(const auto* component=object->getComponent<entity::MeshComponent>();component && !component->meshPath.empty()){
            const auto mesh=resource::ResourceManager::instance().load<resource::IMesh>(resource::resolveAssetPath({},component->meshPath));
            if(mesh && mesh->hasBounds()){a=mesh->getBounds().getMin();b=mesh->getBounds().getMax();}
        }
        if(!finite(a)||!finite(b)||a.x>b.x||a.y>b.y||a.z>b.z)return false;
        const auto matrix=object->worldMatrix();
        for(unsigned corner=0;corner<8;++corner){
            const auto p=matrix.transformPoint({corner&1?b.x:a.x,corner&2?b.y:a.y,corner&4?b.z:a.z});
            if(!finite(p))return false;
            low.x=std::min(low.x,p.x);low.y=std::min(low.y,p.y);low.z=std::min(low.z,p.z);
            high.x=std::max(high.x,p.x);high.y=std::max(high.y,p.y);high.z=std::max(high.z,p.z);
        }
    }
    const auto center=low*.5f+high*.5f;
    const float radius=std::max(.05f,(high-low).length()*.5f);
    const float aspect=_sceneCamera.viewportAspect();
    if(!finite(center)||!std::isfinite(radius)||!std::isfinite(aspect)||aspect<=0)return false;
    const float halfFov=_sceneCamera.threeD().fovYDegrees()*.00872664625997164788f;
    const float narrowHalfFov=std::atan(std::tan(halfFov)*std::min(1.f,aspect));
    const bool ortho=_sceneCamera.threeDProjection()==ProjectionMode::Orthographic;
    const float distance=ortho ? radius*3.f+1.f : radius/std::sin(narrowHalfFov)*1.15f;
    // The existing Scene camera uses a 1000-meter far plane. Refuse instead
    // of claiming to frame a selection outside that current display range.
    if(!std::isfinite(distance)||distance+radius>990.f)return false;
    auto state=_sceneCamera.state();
    state.threeDEye=center-_sceneCamera.threeD().forward()*distance;
    if(!finite(state.threeDEye)||(state.threeDEye-center).lengthSq()<.001f)return false;
    if(ortho)state.threeDOrthoHeight=2.f*radius*1.15f/std::min(1.f,aspect);
    _sceneCamera.restoreState(state);
    _sceneViewWorkspaceDirty=true;_sceneViewWorkspaceSaveCountdown=.5f;
    pushSceneCameraToRenderer();syncTransformGizmoToRenderer();
    if(_repaintCallback)_repaintCallback();
    return true;
}
} // namespace ayt::editor
