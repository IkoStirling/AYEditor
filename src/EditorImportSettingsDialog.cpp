#include "EditorImportSettingsDialog.h"
#include <AYUI/Authoring/PropertyField.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/TextInput.h>
#include <AYUI/Tooltip.h>
#include <AYUI/ScrollView.h>
#include <algorithm>
#include <filesystem>

namespace ayt::editor {
using namespace ayt::ui;
using namespace ayt::resource;

EditorImportSettingsDialog::EditorImportSettingsDialog(ImportOptions options, Apply apply, Localize localize,
                                                     PickSource pickSource)
    : _original(std::move(options)), _apply(std::move(apply)), _localize(std::move(localize)),
      _pickSource(std::move(pickSource)), _sourcePath(_original.sourcePath)
{
    setId("asset_import_settings_dialog");
    setSize({720, _pickSource ? 672.0f : 640.0f});
    setAcceptText(_localize("ui.editor.import.apply", L"Apply"));
    setRejectText(_localize("ui.editor.import.cancel", L"Cancel"));
    auto* body = new VBox();
    body->setSize({688, _pickSource ? 600.0f : 568.0f});
    body->setSpacing(4);
    auto* title = new TextLabel();
    title->setText(_localize("ui.editor.import.title", L"Import Settings"));
    title->setFontSize(16);
    body->addWidget(title, 26);
    const auto field = [&](const char* id) {
        authoring::PropertyFieldOptions settings;
        settings.inputId = std::string("import_settings_") + id + "_input";
        settings.choiceId = std::string("import_settings_") + id;
        settings.labelWidth = 190;
        settings.submitOnChoice = false;
        auto* row = new authoring::PropertyField(settings);
        body->addWidget(row, 28);
        _fields.emplace(id, row);
        return row;
    };
    const auto choice = [&](const char* id, const char* key, const wchar_t* label,
                             int selected, const std::vector<std::wstring>& values) {
        auto* row = field(id);
        row->setChoiceValue(_localize(key, label), values.at(static_cast<size_t>(selected)), values);
    };
    auto* sourceField = field("source");
    updateSourceDisplay();
    if (_pickSource) {
        auto* browse = new Button();
        browse->setId("import_settings_relink");
        browse->setText(_localize("ui.editor.import.relink", L"Locate source file..."));
        browse->setOnClicked([this] { chooseSource(); });
        body->addWidget(browse, 28);
    }
    const auto ext = importExtensionOf(_original.sourcePath);
    const bool model = ext == "fbx" || ext == "gltf" || ext == "glb";
    if (model) {
        choice("content", "ui.editor.import.content", L"Content", int(_original.loadOption),
            {_localize("ui.editor.import.mesh_only", L"Meshes only"),
             _localize("ui.editor.import.full", L"Full scene"),
             _localize("ui.editor.import.animation_only", L"Animations only")});
        choice("coordinates", "ui.editor.import.coordinates", L"Coordinates", int(_original.sourceCoordinates.mode),
            {_localize("ui.editor.import.auto", L"Automatic"), _localize("ui.editor.import.manual", L"Manual")});
        const std::vector<std::wstring> axes{L"+X", L"-X", L"+Y", L"-Y", L"+Z", L"-Z"};
        choice("up", "ui.editor.import.up", L"Source up", int(_original.sourceCoordinates.up), axes);
        choice("forward", "ui.editor.import.forward", L"Source forward", int(_original.sourceCoordinates.forward), axes);
        choice("handedness", "ui.editor.import.handedness", L"Source handedness", int(_original.sourceCoordinates.handedness),
            {_localize("ui.editor.import.left", L"Left-handed"), _localize("ui.editor.import.right", L"Right-handed")});
        field("units")->setTextValue(_localize("ui.editor.import.units", L"Meters per unit (0 = auto)"),
            authoring::formatNumber(_original.sourceCoordinates.metersPerUnit, 9));
        choice("uv", "ui.editor.import.uv", L"Source UV origin", int(_original.sourceCoordinates.uvOrigin),
            {_localize("ui.editor.import.top", L"Top-left"), _localize("ui.editor.import.bottom", L"Bottom-left")});
        choice("normal_y", "ui.editor.import.normal_y", L"Normal map Y", _original.materialPolicy.normalMapYSign < 0 ? 1 : 0,
            {L"+Y (OpenGL)", L"-Y (DirectX)"});
    }
    choice("textures", "ui.editor.import.textures", L"Texture output", _original.cookTextures ? 1 : 0,
        {_localize("ui.editor.import.raw", L"Original images"), _localize("ui.editor.import.cooked", L"Cooked textures")});
    choice("sampling", "ui.editor.import.sampling", L"Texture filtering",
        _original.overrideTextureSampler ? std::max(1,int(_original.textureSampler.minFilter)) : 0,
        {_localize("ui.editor.import.sampling_source", L"From asset"),
         _localize("ui.editor.import.sampling_point", L"Nearest"),
         _localize("ui.editor.import.sampling_bilinear", L"Bilinear"),
         _localize("ui.editor.import.sampling_trilinear", L"Trilinear"),
         _localize("ui.editor.import.sampling_anisotropic", L"Anisotropic")});
    choice("sampling_mag", "ui.editor.import.sampling_mag", L"Magnification",
        _original.textureSampler.magFilter == TextureFilter::Point || _original.textureSampler.magFilter == TextureFilter::None ? 0 : 1,
        {_localize("ui.editor.import.sampling_point", L"Nearest"), _localize("ui.editor.import.sampling_linear", L"Linear")});
    const std::vector<std::wstring> wraps{
        _localize("ui.editor.import.wrap_repeat", L"Repeat"), _localize("ui.editor.import.wrap_clamp", L"Clamp"),
        _localize("ui.editor.import.wrap_mirror", L"Mirror"), _localize("ui.editor.import.wrap_border", L"Border")};
    choice("sampling_u", "ui.editor.import.sampling_u", L"Texture wrap U", int(_original.textureSampler.wrapU), wraps);
    choice("sampling_v", "ui.editor.import.sampling_v", L"Texture wrap V", int(_original.textureSampler.wrapV), wraps);
    choice("texture_mips", "ui.editor.import.texture_mips", L"Mipmaps", _original.keepTextureMipmaps ? 0 : 1,
        {_localize("ui.editor.import.mips_preserve", L"Keep available levels"),
         _localize("ui.editor.import.mips_base", L"Base level only")});
    const auto refreshSamplingFields = [this] {
        const int mode = _fields.at("sampling")->choice()->getSelectedIndex();
        const bool enabled = mode > 0;
        for (const auto* id : {"sampling_mag","sampling_u","sampling_v"}) _fields.at(id)->choice()->setEnabled(enabled);
        if (mode == int(TextureFilter::Anisotropic)) {
            _fields.at("sampling_mag")->choice()->setSelectedIndex(1);
            _fields.at("sampling_mag")->choice()->setEnabled(false);
        }
    };
    _fields.at("sampling")->setOnEdited(refreshSamplingFields);
    refreshSamplingFields();
    if (model) {
        field("opaque")->setTextValue(_localize("ui.editor.import.opaque", L"Opaque material names"), decodeUtf8Text(_original.materialPolicy.opaqueNames));
        field("mask")->setTextValue(_localize("ui.editor.import.mask", L"Cutout material names"), decodeUtf8Text(_original.materialPolicy.maskNames));
        field("blend")->setTextValue(_localize("ui.editor.import.blend", L"Transparent material names"), decodeUtf8Text(_original.materialPolicy.blendNames));
        field("double_sided")->setTextValue(_localize("ui.editor.import.double_sided", L"Double-sided names"), decodeUtf8Text(_original.materialPolicy.doubleSidedNames));
    }
    auto* hint = new TextLabel();
    hint->setWordWrap(true);
    hint->setText(model
        ? _localize("ui.editor.import.model_hint", L"Manual axes apply in Manual mode. Separate material names with ;. Settings stay with the project when its cache is cleared.")
        : _localize("ui.editor.import.image_hint", L"Changing texture output format creates a new imported asset path. Settings stay with the project when its cache is cleared."));
    body->addWidget(hint, 36);
    auto* samplingHint = new TextLabel();
    samplingHint->setWordWrap(true);
    samplingHint->setText(_localize("ui.editor.import.sampling_hint",
        L"Filtering and wrap overrides need cooked textures. Anisotropic uses the device maximum. Border uses edge clamping. Missing mip levels are not generated for DDS."));
    body->addWidget(samplingHint, 48);
    _error = new TextLabel();
    _error->setId("import_settings_error");
    _error->setTextColor({1, .4f, .3f, 1});
    _error->setWordWrap(true);
    auto* scroll = new ScrollView();
    scroll->setId("import_settings_scroll");
    scroll->setHorizontalScrollBarEnabled(false);
    scroll->setContentOwned(body);
    auto* outer = new VBox();
    outer->setSize({688, _pickSource ? 600.0f : 568.0f});
    outer->setSpacing(4);
    outer->addWidget(scroll); // Flex: leave validation errors visible below the scroll area.
    outer->addWidget(_error,32);
    setBodyContentOwned(outer);
    _sourceTooltip = Tooltip::attachTo(sourceField->input());
    if (_sourceTooltip) _sourceTooltip->setText(decodeUtf8Text(_original.sourcePath));
    setAcceptValidator([this] { return applyChanges(); });
}

EditorImportSettingsDialog::~EditorImportSettingsDialog()
{
    if (_sourceTooltip) {
        _sourceTooltip->detach();
        destroyWidgetTree(_sourceTooltip);
        _sourceTooltip = nullptr;
    }
}

void EditorImportSettingsDialog::updateSourceDisplay()
{
    const auto name = std::filesystem::u8path(_sourcePath).filename().generic_u8string();
    auto display = decodeUtf8Text(std::string(reinterpret_cast<const char*>(name.data()), name.size()));
    if (display.size() > 32) {
        std::size_t cut=29;
        if (display[cut-1]>=0xd800 && display[cut-1]<=0xdbff) --cut;
        display=display.substr(0,cut)+L"...";
    }
    _fields.at("source")->setTextValue(_localize("ui.editor.import.source", L"Source"),display,true);
    if (_sourceTooltip) _sourceTooltip->setText(decodeUtf8Text(_sourcePath));
}

void EditorImportSettingsDialog::chooseSource()
{
    try {
        const auto selected=_pickSource();
        if(selected.empty())return;
        if(importExtensionOf(selected)!=importExtensionOf(_original.sourcePath)) {
            _error->setText(_localize("ui.editor.import.relink_type",L"Choose a source with the same file extension."));
            return;
        }
        if(!std::filesystem::is_regular_file(std::filesystem::u8path(selected))) {
            _error->setText(_localize("ui.editor.import.relink_missing",L"The selected source file is missing."));
            return;
        }
        _sourcePath=selected;
        updateSourceDisplay();
        _error->setText({});
    } catch (const std::exception& failure) {
        _error->setText(decodeUtf8Text(failure.what()));
    }
}

bool EditorImportSettingsDialog::applyChanges()
{
    auto candidate = _original;
    if (_sourcePath != _original.sourcePath) {
        candidate.sourcePath = _sourcePath;
        if (candidate.outputBaseName.empty()) {
            const auto stem = std::filesystem::u8path(_original.sourcePath).stem().generic_u8string();
            candidate.outputBaseName.assign(reinterpret_cast<const char*>(stem.data()), stem.size());
        }
    }
    candidate.cookTextures = _fields.at("textures")->choice()->getSelectedIndex() == 1;
    const int sampling = _fields.at("sampling")->choice()->getSelectedIndex();
    candidate.overrideTextureSampler = sampling > 0;
    candidate.keepTextureMipmaps = _fields.at("texture_mips")->choice()->getSelectedIndex() == 0;
    if (candidate.overrideTextureSampler) {
        candidate.textureSampler.minFilter = static_cast<TextureFilter>(sampling);
        candidate.textureSampler.magFilter = _fields.at("sampling_mag")->choice()->getSelectedIndex() == 0
            ? TextureFilter::Point : TextureFilter::Bilinear;
        candidate.textureSampler.wrapU = static_cast<TextureWrap>(_fields.at("sampling_u")->choice()->getSelectedIndex());
        candidate.textureSampler.wrapV = static_cast<TextureWrap>(_fields.at("sampling_v")->choice()->getSelectedIndex());
        if (candidate.textureSampler.minFilter == TextureFilter::Anisotropic) candidate.textureSampler.anisotropy = 16;
    }
    const auto extension = importExtensionOf(candidate.sourcePath);
    if (!candidate.cookTextures && extension != "dds"
        && (candidate.overrideTextureSampler || !candidate.keepTextureMipmaps)) {
        _error->setText(_localize("ui.editor.import.sampling_needs_cooked",
            L"Choose Cooked textures to save texture filtering, wrap or mipmap overrides."));
        return false;
    }
    if (_fields.contains("content")) {
        candidate.loadOption = static_cast<IConverter::LoadOption>(_fields.at("content")->choice()->getSelectedIndex());
        candidate.requireAnimationAssets = candidate.loadOption == IConverter::LoadOption::AnimationOnly;
        if (candidate.loadOption != IConverter::LoadOption::Full) candidate.requireCharacterAssets = false;
        auto& coordinates = candidate.sourceCoordinates;
        coordinates.mode = static_cast<SourceCoordinateMode>(_fields.at("coordinates")->choice()->getSelectedIndex());
        coordinates.up = static_cast<ImportAxis>(_fields.at("up")->choice()->getSelectedIndex());
        coordinates.forward = static_cast<ImportAxis>(_fields.at("forward")->choice()->getSelectedIndex());
        coordinates.handedness = static_cast<ImportHandedness>(_fields.at("handedness")->choice()->getSelectedIndex());
        coordinates.uvOrigin = static_cast<ImportUvOrigin>(_fields.at("uv")->choice()->getSelectedIndex());
        const auto units = authoring::parseFiniteFloat(_fields.at("units")->value());
        if (!units || *units < 0) {
            _error->setText(_localize("ui.editor.import.invalid_units", L"Meters per unit must be a finite number greater than or equal to zero."));
            return false;
        }
        coordinates.metersPerUnit = *units;
        if (coordinates.mode == SourceCoordinateMode::Manual && int(coordinates.up) / 2 == int(coordinates.forward) / 2) {
            _error->setText(_localize("ui.editor.import.invalid_axes", L"Up and Forward must use different source axes."));
            return false;
        }
        auto& material = candidate.materialPolicy;
        material.normalMapYSign = _fields.at("normal_y")->choice()->getSelectedIndex() == 1 ? -1.0f : 1.0f;
        material.opaqueNames = encodeUtf8Text(_fields.at("opaque")->value());
        material.maskNames = encodeUtf8Text(_fields.at("mask")->value());
        material.blendNames = encodeUtf8Text(_fields.at("blend")->value());
        material.doubleSidedNames = encodeUtf8Text(_fields.at("double_sided")->value());
    }
    std::string error;
    if (!_apply(candidate, error)) { _error->setText(decodeUtf8Text(error)); return false; }
    _original = std::move(candidate);
    _error->setText({});
    return true;
}
} // namespace ayt::editor
