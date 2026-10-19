#include "AYEditor/EditorSession.h"
#include "AYRenderer/ColorStylizationLook.h"
#include "AYRenderer/RendererSubSystem.h"
#include "AYRenderer.h"
#include "AYUI/CheckBox.h"
#include "AYUI/ComboBox.h"
#include "AYUI/Slider.h"
#include "AYUI/TextInput.h"
#include "AYUI/TextLabel.h"
#include "AYUI/Button.h"
#include "AYUI/ColorPicker.h"
#include "AYUI/UnicodeText.h"
#include <filesystem>
#include <cctype>
#include <sstream>
#include <string_view>

namespace ayt::editor {
namespace {
using namespace ayt::render;
namespace fs=std::filesystem;
template<class T> T* widget(const ayt::ui::UIManager& ui,const char* id) {
    return dynamic_cast<T*>(ui.findById(id));
}
void status(const ayt::ui::UIManager& ui,const std::string& text) {
    if(auto* label=widget<ayt::ui::TextLabel>(ui,"lbl_style_status"))
        label->setText(ayt::ui::decodeUtf8Text(text));
}
void updateVisibility(ayt::ui::UIManager& ui,const ColorStylizationSettings& s) {
    const auto show=[&](const char* id,bool visible){if(auto* w=ui.findById(id))w->setVisible(visible);};
    const auto pair=[&](const char* name,bool visible){
        const std::string label=std::string("lbl_style_")+name;
        const std::string slider=std::string("sld_style_")+name;
        show(label.c_str(),visible);show(slider.c_str(),visible);
    };
    pair("pixel_size",s.pixelationEnabled);
    const bool palette=s.colorMapping==StylizationColorMapping::FixedPalette;
    const bool duo=s.colorMapping==StylizationColorMapping::Duotone;
    show("chk_style_posterization",!palette);pair("color_levels",!palette && s.posterizationEnabled);
    pair("dither_strength",palette || s.posterizationEnabled);
    for(const char* name:{"shadow","highlight","palette"}) {
        const bool visible=std::string_view(name)=="palette"?palette:duo;
        const std::string label=std::string("lbl_style_")+name,input=std::string("txt_style_")+name;
        show(label.c_str(),visible);show(input.c_str(),visible);
    }
    const bool print=s.printPattern!=StylizationPrintPattern::None;
    for(const char* name:{"print_spacing","print_angle","print_strength","print_gamma"})pair(name,print);
    for(const char* name:{"ink","paper"}) {
        const std::string label=std::string("lbl_style_")+name,input=std::string("txt_style_")+name;
        show(label.c_str(),print);show(input.c_str(),print);
    }
    const bool outline=s.outlineMode!=StylizationOutlineMode::None;
    pair("outline_width",outline);pair("outline_strength",outline);
    pair("outline_luminance",s.outlineMode==StylizationOutlineMode::Luminance);
    pair("outline_depth",s.outlineMode>=StylizationOutlineMode::Depth);
    pair("outline_normal",s.outlineMode==StylizationOutlineMode::DepthNormal);
    show("lbl_style_normal_hint",s.outlineMode==StylizationOutlineMode::DepthNormal);
    show("lbl_style_outline",outline);show("txt_style_outline",outline);
    for(const char* name:{"scanline","mask","chroma","curvature","vignette","grain","noise_seed"})pair(name,s.display.enabled);
    const bool temporal=s.temporal.enabled,afterimage=s.temporal.mode==TemporalFeedbackMode::Afterimage;
    show("lbl_style_temporal_mode",temporal);show("cmb_style_temporal_mode",temporal);
    pair("history_weight",temporal && afterimage);
    pair("history_offset_x",temporal && afterimage);pair("history_offset_y",temporal && afterimage);
    pair("hold_frames",temporal && !afterimage);
    pair("tile_size",temporal && s.temporal.mode==TemporalFeedbackMode::TileHold);
    pair("temporal_seed",temporal && s.temporal.mode==TemporalFeedbackMode::TileHold);
    show("btn_style_history_reset",temporal);
    ui.invalidateLayout();
}
bool projectLookPath(const std::string& root,const std::string& request,std::string& path,std::string* error) {
    const auto fail=[&](const char* text){if(error)*error=text;return false;};
    if(error)error->clear();
    if(root.empty() || request.empty())return fail("Open a project and choose a look path first");
    try {
        const auto base=fs::weakly_canonical(fs::u8path(root));
        auto target=fs::u8path(request);if(target.is_relative())target=base/target;
        target=fs::weakly_canonical(target);
        const auto relative=target.lexically_relative(base);
        if(relative.empty() || relative.is_absolute() || *relative.begin()=="..")
            return fail("Look path must remain inside the current project");
        auto ext=target.extension().string();
        for(auto& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(ext!=".rsl")return fail("Look documents must use the .rsl suffix");
        const auto utf8=target.u8string();path.assign(utf8.begin(),utf8.end());return true;
    }catch(const std::exception&){return fail("Invalid look path");}
}
}

ayt::render::ColorStylizationSettings EditorSession::renderStyleLook() const {
    auto s=_renderStyleLook;
    const auto check=[&](const char* id,bool value){auto* w=widget<ayt::ui::CheckBox>(_ui,id);return w?w->isChecked():value;};
    const auto slider=[&](const char* id,float value){auto* w=widget<ayt::ui::Slider>(_ui,id);return w?w->getValue():value;};
    const auto combo=[&](const char* id,int value){auto* w=widget<ayt::ui::ComboBox>(_ui,id);return w?w->getSelectedIndex():value;};
    s.enabled=check("chk_color_stylization",s.enabled);
    s.pixelationEnabled=check("chk_style_pixelation",s.pixelationEnabled);
    s.posterizationEnabled=check("chk_style_posterization",s.posterizationEnabled);
    s.pixelSize=static_cast<uint16_t>(std::lround(std::clamp(slider("sld_style_pixel_size",s.pixelSize),1.f,64.f)));
    s.colorLevels=static_cast<uint8_t>(std::lround(std::clamp(slider("sld_style_color_levels",s.colorLevels),2.f,32.f)));
    s.ditherStrength=slider("sld_style_dither_strength",s.ditherStrength);
    s.display.enabled=check("chk_style_display",s.display.enabled);
    s.display.scanlineStrength=slider("sld_style_scanline",s.display.scanlineStrength);
    s.display.maskStrength=slider("sld_style_mask",s.display.maskStrength);
    s.display.chromaticOffset=slider("sld_style_chroma",s.display.chromaticOffset);
    s.display.curvature=slider("sld_style_curvature",s.display.curvature);
    s.display.vignetteStrength=slider("sld_style_vignette",s.display.vignetteStrength);
    s.display.grainStrength=slider("sld_style_grain",s.display.grainStrength);
    s.display.noiseSeed=static_cast<uint16_t>(std::lround(std::clamp(slider("sld_style_noise_seed",s.display.noiseSeed),0.f,65535.f)));
    s.temporal.enabled=check("chk_style_temporal",s.temporal.enabled);
    s.temporal.mode=static_cast<TemporalFeedbackMode>(combo("cmb_style_temporal_mode",int(s.temporal.mode)));
    s.temporal.historyWeight=slider("sld_style_history_weight",s.temporal.historyWeight);
    s.temporal.holdFrames=static_cast<uint16_t>(std::lround(std::clamp(slider("sld_style_hold_frames",s.temporal.holdFrames),1.f,120.f)));
    s.temporal.tileSize=static_cast<uint16_t>(std::lround(std::clamp(slider("sld_style_tile_size",s.temporal.tileSize),4.f,128.f)));
    s.temporal.historyOffsetX=slider("sld_style_history_offset_x",s.temporal.historyOffsetX);
    s.temporal.historyOffsetY=slider("sld_style_history_offset_y",s.temporal.historyOffsetY);
    s.temporal.noiseSeed=static_cast<uint16_t>(std::lround(std::clamp(slider("sld_style_temporal_seed",s.temporal.noiseSeed),0.f,65535.f)));
    s.colorMapping=static_cast<StylizationColorMapping>(combo("cmb_style_mapping",int(s.colorMapping)));
    s.printPattern=static_cast<StylizationPrintPattern>(combo("cmb_style_print",int(s.printPattern)));
    s.outlineMode=static_cast<StylizationOutlineMode>(combo("cmb_style_outline",int(s.outlineMode)));
#define READ_STYLE_SLIDER(field,id) s.field=slider(id,s.field)
    READ_STYLE_SLIDER(printSpacing,"sld_style_print_spacing");READ_STYLE_SLIDER(printAngle,"sld_style_print_angle");
    READ_STYLE_SLIDER(printStrength,"sld_style_print_strength");READ_STYLE_SLIDER(outlineWidth,"sld_style_outline_width");
    READ_STYLE_SLIDER(printGamma,"sld_style_print_gamma");
    READ_STYLE_SLIDER(outlineDepthThreshold,"sld_style_outline_depth");
    READ_STYLE_SLIDER(outlineNormalThreshold,"sld_style_outline_normal");
    READ_STYLE_SLIDER(outlineLuminanceThreshold,"sld_style_outline_luminance");
    READ_STYLE_SLIDER(outlineStrength,"sld_style_outline_strength");
#undef READ_STYLE_SLIDER
    return sanitizeColorStylization(s);
}

void EditorSession::syncColorStylizationPanel() {
    const bool previous=_syncingRenderStyle;_syncingRenderStyle=true;
    const auto& s=_renderStyleLook;
    const auto check=[&](const char* id,bool value){if(auto* w=widget<ayt::ui::CheckBox>(_ui,id))w->setChecked(value);};
    const auto slider=[&](const char* id,float value){if(auto* w=widget<ayt::ui::Slider>(_ui,id))w->setValue(value);};
    const auto combo=[&](const char* id,int value){if(auto* w=widget<ayt::ui::ComboBox>(_ui,id))w->setSelectedIndex(value);};
    check("chk_color_stylization",s.enabled);check("chk_style_pixelation",s.pixelationEnabled);
    check("chk_style_posterization",s.posterizationEnabled);
    slider("sld_style_pixel_size",s.pixelSize);slider("sld_style_color_levels",s.colorLevels);
    slider("sld_style_dither_strength",s.ditherStrength);
    check("chk_style_display",s.display.enabled);
    slider("sld_style_scanline",s.display.scanlineStrength);slider("sld_style_mask",s.display.maskStrength);
    slider("sld_style_chroma",s.display.chromaticOffset);slider("sld_style_curvature",s.display.curvature);
    slider("sld_style_vignette",s.display.vignetteStrength);slider("sld_style_grain",s.display.grainStrength);
    slider("sld_style_noise_seed",s.display.noiseSeed);
    check("chk_style_temporal",s.temporal.enabled);combo("cmb_style_temporal_mode",int(s.temporal.mode));
    slider("sld_style_history_weight",s.temporal.historyWeight);slider("sld_style_hold_frames",s.temporal.holdFrames);
    slider("sld_style_tile_size",s.temporal.tileSize);slider("sld_style_history_offset_x",s.temporal.historyOffsetX);
    slider("sld_style_history_offset_y",s.temporal.historyOffsetY);slider("sld_style_temporal_seed",s.temporal.noiseSeed);
    combo("cmb_style_mapping",int(s.colorMapping));combo("cmb_style_print",int(s.printPattern));
    combo("cmb_style_outline",int(s.outlineMode));
#define SYNC_STYLE_SLIDER(field,id) slider(id,s.field)
    SYNC_STYLE_SLIDER(printSpacing,"sld_style_print_spacing");SYNC_STYLE_SLIDER(printAngle,"sld_style_print_angle");
    SYNC_STYLE_SLIDER(printStrength,"sld_style_print_strength");SYNC_STYLE_SLIDER(outlineWidth,"sld_style_outline_width");
    SYNC_STYLE_SLIDER(printGamma,"sld_style_print_gamma");
    SYNC_STYLE_SLIDER(outlineDepthThreshold,"sld_style_outline_depth");
    SYNC_STYLE_SLIDER(outlineNormalThreshold,"sld_style_outline_normal");
    SYNC_STYLE_SLIDER(outlineLuminanceThreshold,"sld_style_outline_luminance");
    SYNC_STYLE_SLIDER(outlineStrength,"sld_style_outline_strength");
#undef SYNC_STYLE_SLIDER
    const auto color=[&](const char* id,StylizationColor value){
        if(auto* w=widget<ayt::ui::TextInput>(_ui,id))
            w->setText(ayt::ui::ColorPicker::formatHexCode({value.r,value.g,value.b,1}).substr(0,7));
    };
    color("txt_style_shadow",s.shadowColor);color("txt_style_highlight",s.highlightColor);
    color("txt_style_ink",s.inkColor);color("txt_style_paper",s.paperColor);color("txt_style_outline",s.outlineColor);
    if(auto* w=widget<ayt::ui::TextInput>(_ui,"txt_style_palette")) {
        std::wstring text;for(uint8_t i=0;i<s.paletteSize;++i) {
            if(i)text+=L" ";auto c=s.palette[i];
            text+=ayt::ui::ColorPicker::formatHexCode({c.r,c.g,c.b,1}).substr(0,7);
        }w->setText(text);
    }
    updateVisibility(_ui,s);_syncingRenderStyle=previous;refreshLocalizedValueLabels();
}

void EditorSession::setRenderStyleLook(const ColorStylizationSettings& settings) {
    _renderStyleLook=sanitizeColorStylization(settings);syncColorStylizationPanel();
    if(auto* sub=RendererSubSystem::findRegistered())sub->renderer().setColorStylizationSettings(_renderStyleLook);
}
void EditorSession::applyColorStylizationFromPanel() {
    if(_syncingRenderStyle)return;
    _renderStyleLook=renderStyleLook();
    updateVisibility(_ui,_renderStyleLook);
    if(auto* sub=RendererSubSystem::findRegistered())sub->renderer().setColorStylizationSettings(_renderStyleLook);
}
bool EditorSession::saveRenderStyleLook(const std::string& request,std::string* error) {
    std::string path;if(!projectLookPath(_projectRoot,request,path,error))return false;
    return saveColorStylizationLook(path,renderStyleLook(),error);
}
bool EditorSession::loadRenderStyleLook(const std::string& request,std::string* error) {
    std::string path;if(!projectLookPath(_projectRoot,request,path,error))return false;
    ColorStylizationSettings loaded;if(!loadColorStylizationLook(path,loaded,error))return false;
    setRenderStyleLook(loaded);return true;
}

void EditorSession::bindColorStylizationPanel() {
    const auto changed=[this]{if(!_applyingPreferences && !_syncingRenderStyle){applyColorStylizationFromPanel();refreshLocalizedValueLabels();}};
    for(const char* id:{"chk_color_stylization","chk_style_pixelation","chk_style_posterization","chk_style_display","chk_style_temporal"})
        if(auto* w=widget<ayt::ui::CheckBox>(_ui,id))w->setOnToggled([changed](bool){changed();});
    for(const char* id:{"cmb_style_mapping","cmb_style_print","cmb_style_outline","cmb_style_temporal_mode"})
        if(auto* w=widget<ayt::ui::ComboBox>(_ui,id))w->setOnSelectionChanged([changed](int){changed();});
    if(auto* w=widget<ayt::ui::ComboBox>(_ui,"cmb_color_stylization_preset"))
        w->setOnSelectionChanged([this](int index){
            if(_applyingPreferences || _syncingRenderStyle)return;
            setRenderStyleLook(colorStylizationPreset(static_cast<ColorStylizationPreset>(std::clamp(index,0,15))));
        });
    for(const char* id:{"sld_style_pixel_size","sld_style_color_levels","sld_style_dither_strength",
        "sld_style_print_spacing","sld_style_print_angle","sld_style_print_strength","sld_style_print_gamma","sld_style_outline_width",
        "sld_style_outline_depth","sld_style_outline_normal","sld_style_outline_luminance","sld_style_outline_strength",
        "sld_style_scanline","sld_style_mask","sld_style_chroma","sld_style_curvature","sld_style_vignette","sld_style_grain","sld_style_noise_seed",
        "sld_style_history_weight","sld_style_hold_frames","sld_style_tile_size","sld_style_history_offset_x","sld_style_history_offset_y","sld_style_temporal_seed"})
        if(auto* w=widget<ayt::ui::Slider>(_ui,id))w->setOnValueChanged([changed](float){changed();});
    using Member=StylizationColor ColorStylizationSettings::*;
    for(const auto& [id,member]:std::initializer_list<std::pair<const char*,Member>>{
        {"txt_style_shadow",&ColorStylizationSettings::shadowColor},{"txt_style_highlight",&ColorStylizationSettings::highlightColor},
        {"txt_style_ink",&ColorStylizationSettings::inkColor},{"txt_style_paper",&ColorStylizationSettings::paperColor},
        {"txt_style_outline",&ColorStylizationSettings::outlineColor}}) {
        if(auto* w=widget<ayt::ui::TextInput>(_ui,id)) {
            const auto commit=[this,w,member] {
                if(_syncingRenderStyle || _applyingPreferences)return;
                ayt::math::FVector4 c;
                if(w->getText().size()!=7 || !ayt::ui::ColorPicker::parseHexCode(w->getText(),c)) {status(_ui,"Use #RRGGBB");return;}
                _renderStyleLook.*member={c.x,c.y,c.z};applyColorStylizationFromPanel();status(_ui,"");
            };
            w->setOnSubmit([commit](const std::wstring&){commit();});w->setOnFocusLostNotify(commit);
        }
    }
    if(auto* w=widget<ayt::ui::TextInput>(_ui,"txt_style_palette")) {
        const auto commit=[this,w] {
            if(_syncingRenderStyle || _applyingPreferences)return;
            auto palette=_renderStyleLook.palette;std::wistringstream input(w->getText());
            std::wstring token;uint8_t count=0;
            while(input>>token) {
                ayt::math::FVector4 c;
                if(count==16 || token.size()!=7 || !ayt::ui::ColorPicker::parseHexCode(token,c)) {status(_ui,"Use 2..16 space-separated #RRGGBB colors");return;}
                palette[count++]={c.x,c.y,c.z};
            }
            if(count<2){status(_ui,"A palette needs at least two colors");return;}
            _renderStyleLook.palette=palette;_renderStyleLook.paletteSize=count;
            applyColorStylizationFromPanel();status(_ui,"");
        };
        w->setOnSubmit([commit](const std::wstring&){commit();});w->setOnFocusLostNotify(commit);
    }
    if(auto* w=widget<ayt::ui::Button>(_ui,"btn_style_reset"))w->setOnClicked([this]{setRenderStyleLook({});status(_ui,"Look reset");});
    if(auto* w=widget<ayt::ui::Button>(_ui,"btn_style_history_reset"))w->setOnClicked([this]{
        if(auto* sub=RendererSubSystem::findRegistered()){sub->renderer().resetStylizationHistory();status(_ui,"Image history cleared");}
    });
    for(bool save:{false,true})if(auto* w=widget<ayt::ui::Button>(_ui,save?"btn_style_save":"btn_style_load")) {
        w->setOnClicked([this,save]{
            const auto* input=widget<ayt::ui::TextInput>(_ui,"txt_style_path");if(!input)return;
            const auto utf8=fs::path(input->getText()).u8string();const std::string path(utf8.begin(),utf8.end());
            std::string error;const bool ok=save?saveRenderStyleLook(path,&error):loadRenderStyleLook(path,&error);
            status(_ui,ok?(save?"Look saved":"Look loaded"):error);
        });
    }
}
} // namespace ayt::editor
