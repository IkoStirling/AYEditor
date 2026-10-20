#include <AYEditor/EditorMaterialDocument.h>
#include <AYEditor/EditorCommandButtons.h>
#include <AYEditor/EditorProjectDescriptor.h>
#include <AYRenderer.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYRenderer/PreviewScene.h>
#include <AYUI/Authoring/NumericFields.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Authoring/PreviewViewport.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ScrollView.h>
#include <AYUI/TextLabel.h>
#include <AYUI/TextInput.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/ListView.h>
#include <AYUI/UIManager.h>
#include <AYUI/TextArea.h>
#include <AYUI/UnicodeText.h>
#include <AYMath/MathUtils.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace ayt::editor
{
namespace
{
using namespace ui;
using namespace ui::authoring;
class MaterialPreview final : public Widget
{
  public:
    explicit MaterialPreview(std::shared_ptr<EditorMaterialDocument> document)
        : _document(std::move(document))
    {
        setId("material_preview");
        setDisplayListPolicy(DisplayListPolicy::Immediate);
    }
    ~MaterialPreview() override
    {
        shutdown();
    }
    void shutdown()
    {
        if (_ui && _ui->isInitialized())
            _ui->releaseRenderTarget(_target);
        if (_renderer && _renderer->isInitialized())
        {
            _renderer->destroyMaterial(_material);
            _renderer->destroyMesh(_sphere);
            _renderer->destroyMesh(_tile);
        }
        _ui = nullptr;
        _renderer = nullptr;
        _target = {};
        _material = {};
        _sphere = {};
        _tile = {};
        _revision = UINT64_MAX;
    }
    void setShape(int shape)
    {
        _shape = shape;
        markDirty();
    }
    void resetCamera()
    {
        _orbit.reset();
        markDirty();
    }
    bool onMouseButtonDown(const UIMouseEvent &e) override
    {
        if (e.mouseButton != 0)
            return false;
        _orbit.begin(e.mousePos);
        return true;
    }
    bool onMouseMove(const UIMouseEvent &e) override
    {
        if (!_orbit.move(e.mousePos))
            return false;
        markDirty();
        return true;
    }
    bool onMouseButtonUp(const UIMouseEvent &e) override
    {
        return e.mouseButton == 0 && _orbit.end();
    }
    bool onMouseWheel(const UIMouseWheelEvent &e) override
    {
        _orbit.wheel(e.deltaY);
        markDirty();
        return true;
    }
    void onCaptureCancelled() override
    {
        _orbit.cancel();
    }

  protected:
    void onRender(IRenderBackend &backend) override
    {
        const auto bounds = getWorldBounds();
        auto *gpu = dynamic_cast<render::UIRenderBackend *>(&backend);
        if (!gpu || !gpu->previewRenderer())
        {
            fallback(backend, bounds, L"GPU preview requires the renderer UI backend");
            return;
        }
        auto *renderer = gpu->previewRenderer();
        if (_ui != gpu || _renderer != renderer)
        {
            shutdown();
            _ui = gpu;
            _renderer = renderer;
        }
        const double scaledWidth = (double(bounds.maxX) - bounds.minX) * gpu->getUiScale();
        const double scaledHeight = (double(bounds.maxY) - bounds.minY) * gpu->getUiScale();
        if (!std::isfinite(scaledWidth) || !std::isfinite(scaledHeight))
            return;
        const int w = int(std::ceil(std::clamp(scaledWidth, 1.0, 2048.0)));
        const int h = int(std::ceil(std::clamp(scaledHeight, 1.0, 2048.0)));
        IRenderBackend::RenderTargetDesc desc;
        desc.width = w;
        desc.height = h;
        desc.hasAlpha = false;
        if (!_target.isValid())
            _target = gpu->createRenderTarget(desc);
        else if (_width != w || _height != h)
            if (!gpu->resizeRenderTarget(_target, desc))
            {
                fallback(backend, bounds, L"Preview target resize failed");
                return;
            }
        _width = w;
        _height = h;
        if (!_target.isValid())
        {
            fallback(backend, bounds, L"Preview target unavailable");
            return;
        }
        if (!_sphere.isValid() || !_tile.isValid())
        {
            createGeometry();
            if (!_sphere.isValid() || !_tile.isValid())
            {
                fallback(backend, bounds, L"Preview geometry unavailable");
                return;
            }
        }
        if (_revision != _document->revision())
        {
            std::string error;
            auto next = _document->createPreviewMaterial(*renderer, &error);
            if (!next.isValid())
            {
                fallback(backend, bounds, decodeUtf8Text(error));
                return;
            }
            renderer->destroyMaterial(_material);
            _material = next;
            _revision = _document->revision();
        }
        render::PreviewSceneCamera camera;
        const float distance = 3.3f / _orbit.zoom;
        camera.position = {std::sin(_orbit.yaw) * std::cos(_orbit.pitch) * distance,
                           -std::sin(_orbit.pitch) * distance,
                           -std::cos(_orbit.yaw) * std::cos(_orbit.pitch) * distance};
        camera.view = math::lh::lookAt(camera.position, {0, 0, 0}, {0, 1, 0});
        camera.projection = math::lh::perspective(math::radians(43.f), float(w) / h, .01f, 100);
        render::RenderScene scene;
        scene.add(_shape == 2 ? _tile : _sphere, _material,
                  _shape == 1 ? math::scale(1.7f, .75f, 1.2f) : math::scale(1.5f, 1.5f, 1.5f));
        render::SceneLights lights;
        lights.add(render::Light::directional({-.45f, -.8f, .55f}, {2.2f, 2.05f, 1.9f}));
        lights.add(render::Light::directional({.8f, -.25f, .2f}, {.55f, .7f, 1.1f}));
        lights.add(render::Light::directional({-.3f, -.25f, -.9f}, {1.2f, .8f, .45f}));
        render::PreviewSceneSettings settings;
        settings.lights = &lights;
        settings.displayTransform = true;
        settings.toneMap = render::PreviewToneMap::ACES;
        if (!gpu->renderScenePreview(_target, scene, camera, 0x05090eff, settings))
        {
            fallback(backend, bounds, L"Preview suspended: offscreen view budget");
            return;
        }
        gpu->blitRenderTarget(_target, bounds);
    }

  private:
    static void fallback(IRenderBackend &b, const math::FRectangle &bounds, const std::wstring &text)
    {
        b.drawRect(bounds, {.04f, .05f, .07f, 1});
        b.drawText(bounds, text, 12, math::FVector4{.8f, .8f, .85f, 1});
    }
    struct Vertex
    {
        float x, y, z, nx, ny, nz, tx, ty, tz, tw, u, v;
    };
    void createGeometry()
    {
        constexpr int rows = 24, columns = 48;
        constexpr float pi = 3.14159265358979323846f;
        std::vector<Vertex> vertices;
        std::vector<uint16_t> indices;
        for (int y = 0; y <= rows; ++y)
            for (int x = 0; x <= columns; ++x)
            {
                const float t = pi * y / rows, p = 2 * pi * x / columns, nx = std::sin(t) * std::cos(p),
                            ny = std::cos(t), nz = std::sin(t) * std::sin(p);
                vertices.push_back({nx * .5f, ny * .5f, nz * .5f, nx, ny, nz, -std::sin(p), 0, std::cos(p), 1,
                                    float(x) / columns, float(y) / rows});
            }
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x)
            {
                const auto a = uint16_t(y * (columns + 1) + x), b = uint16_t(a + columns + 1);
                if (y + 1 < rows)
                    indices.insert(indices.end(), {a, uint16_t(b + 1), b});
                if (y > 0)
                    indices.insert(indices.end(), {a, uint16_t(a + 1), uint16_t(b + 1)});
            }
        render::VertexLayoutDesc layout;
        layout.add({render::VertexAttribute::Position, 3, render::VertexComponentType::Float, false});
        layout.add({render::VertexAttribute::Normal, 3, render::VertexComponentType::Float, false});
        layout.add({render::VertexAttribute::Tangent, 4, render::VertexComponentType::Float, false});
        layout.add({render::VertexAttribute::TexCoord0, 2, render::VertexComponentType::Float, false});
        if (!_sphere.isValid())
            _sphere = _renderer->createMesh(vertices.data(), uint32_t(vertices.size()), layout,
                                            indices.data(), uint32_t(indices.size()));
        const Vertex tile[] = {{-.5f, -.5f, 0, 0, 0, -1, 1, 0, 0, 1, 0, 1},
                               {.5f, -.5f, 0, 0, 0, -1, 1, 0, 0, 1, 1, 1},
                               {.5f, .5f, 0, 0, 0, -1, 1, 0, 0, 1, 1, 0},
                               {-.5f, .5f, 0, 0, 0, -1, 1, 0, 0, 1, 0, 0}};
        const uint16_t triangles[] = {0, 2, 1, 0, 3, 2};
        if (!_tile.isValid())
            _tile = _renderer->createMesh(tile, 4, layout, triangles, 6);
    }
    std::shared_ptr<EditorMaterialDocument> _document;
    PreviewOrbit _orbit;
    render::UIRenderBackend *_ui = nullptr;
    render::Renderer *_renderer = nullptr;
    IRenderBackend::RenderTargetHandle _target{};
    render::MeshHandle _sphere{}, _tile{};
    render::MaterialHandle _material{};
    uint64_t _revision = UINT64_MAX;
    int _width = 0, _height = 0, _shape = 0;
};
class MaterialView final : public IEditorView, public IEditorCommandTarget
{
  public:
    MaterialView(std::shared_ptr<EditorMaterialDocument> document, IEditorHostServices &host)
        : _document(std::move(document)), _host(host), _commands([this] { return this; })
    {
        namespace fs = std::filesystem;
        const auto project = EditorProjectDescriptor::load(host.projectRoot());
        const auto root = fs::u8path(host.projectRoot()) / fs::u8path(project ? project.assetRoot : "Assets");
        const auto text = root.u8string();
        _sourceRoot=root;
        _document->configureSourceRoot(std::string(text.begin(), text.end()));
        const auto imported=(fs::u8path(host.projectRoot())/".ayeditor_cache/assets").generic_u8string();
        _document->configureImportedRoot(std::string(reinterpret_cast<const char*>(imported.data()),imported.size()));
        build();
        refresh();
    }
    ~MaterialView() override
    {
        prepareForUiShutdown();
        if (_root)
            destroyWidgetTree(_root);
    }
    Widget *rootWidget() noexcept override
    {
        return _root;
    }
    Widget *releaseRootWidget() noexcept override
    {
        auto *root = _root;
        _root = nullptr;
        return root;
    }
    IEditorCommandTarget *commandTarget() noexcept override
    {
        return this;
    }
    bool handlesCommand(const std::string &id) const override
    {
        return _document->handlesCommand(id);
    }
    bool canExecuteCommand(const std::string &id) const override
    {
        if (_closed)
            return false;
        if (id == "file.save")
            return !_document->readOnly() && (_document->isDirty() || formDirty());
        if (id == "file.reload" && formDirty())
            return false;
        return _document->canExecuteCommand(id);
    }
    bool executeCommand(const std::string &id) override
    {
        if (!canExecuteCommand(id))
            return false;
        if (id == "file.save")
        {
            if (!commit())
                return false;
            std::string error;
            if (_document->isDirty() && !_document->save(&error))
            {
                _status->setText(decodeUtf8Text(error));
                return false;
            }
            refresh();
            _host.requestRepaint();
            return true;
        }
        const bool done = _document->executeCommand(id);
        if (done)
        {
            refresh();
            _host.requestRepaint();
        }
        return done;
    }
    void onLanguageChanged(const std::string&) override
    {
        if (_closed) return;
        _copy->setText(_host.localizedText("ui.editor.material.create_copy", L"Create editable copy..."));
        _copyHint->setText(_host.localizedText("ui.editor.material.copy_hint", L"Create a new material in project assets from this snapshot."));
        for (auto* button : _textureBrowse)
            button->setText(_host.localizedText("ui.editor.material.choose_texture", L"Choose..."));
        _host.requestRepaint();
    }
    void prepareForUiShutdown() override
    {
        if (_closed)
            return;
        _closed = true;
        _commands.detach();
        _copyDialog.reset();_textureDialog.reset();
        if(_copy)_copy->setOnClicked({});
        for(auto* button:_textureBrowse)button->setOnClicked({});
        if (_preview)
            _preview->shutdown();
        for (auto *field : _fields)
        {
            field->setOnSubmitted({});
            for (auto *input : field->inputs())
                input->setOnFocusLostNotify({});
        }
        for (auto *input : _textureFields)
        {
            input->setOnSubmit({});
            input->setOnFocusLostNotify({});
        }
        if (_doubleSided)
            _doubleSided->setOnToggled({});
        if (_alpha)
            _alpha->setOnSelectionChanged({});
        if (_opacitySource)
            _opacitySource->setOnSelectionChanged({});
        if (_shape)
            _shape->setOnSelectionChanged({});
        if (_apply)
            _apply->setOnClicked({});
        if (_reset)
            _reset->setOnClicked({});
    }
    void tick(float) override
    {
        if (_closed)
            return;
        if (_stamp != _document->revision())
            refresh();
        _commands.refresh();
        if (!_document->diagnostic().empty())
            _status->setText(decodeUtf8Text(_document->diagnostic()));
    }

  private:
    HBox *row(VBox &parent, float height = 28)
    {
        auto *r = new HBox();
        r->setSpacing(5);
        parent.addWidget(r, height);
        return r;
    }
    void label(VBox &parent, const std::wstring &text)
    {
        auto *l = new TextLabel();
        l->setText(text);
        parent.addWidget(l, 23);
    }
    NumericFields *fields(VBox &parent, const std::vector<std::string> &ids,
                          const std::vector<std::wstring> &labels)
    {
        auto *f = new NumericFields(ids, labels);
        parent.addWidget(f, 28);
        f->setOnSubmitted([this] { commit(); });
        for (auto *input : f->inputs())
            input->setOnFocusLostNotify([this] { commit(); });
        _fields.push_back(f);
        return f;
    }
    void build()
    {
        _root = new VBox();
        _root->setId("material_editor");
        _root->setSpacing(5);
        auto *toolbar = row(*_root);
        _commands.add(*toolbar, L"Save", "file.save", 70);
        _commands.add(*toolbar, L"Reload", "file.reload", 70);
        _commands.add(*toolbar, L"Undo", "edit.undo", 70);
        _commands.add(*toolbar, L"Redo", "edit.redo", 70);
        _shape = new ComboBox();
        _shape->setId("material_shape");
        _shape->setItems({L"Sphere", L"Scaled Sphere", L"UV Tile"});
        _shape->setSelectedIndex(0);
        toolbar->addWidget(_shape, 150);
        _reset = new Button();
        _reset->setText(L"Reset Camera");
        toolbar->addWidget(_reset, 115);
        auto* copyRow=row(*_root,36);
        _copy=new Button();_copy->setId("material_create_copy");
        _copy->setText(_host.localizedText("ui.editor.material.create_copy",L"Create editable copy..."));
        _copy->setOnClicked([this]{showCopyDialog();});copyRow->addWidget(_copy,210);
        _copyHint=new TextLabel();_copyHint->setWordWrap(true);
        _copyHint->setText(_host.localizedText("ui.editor.material.copy_hint",L"Create a new material in project assets from this snapshot."));
        copyRow->addWidget(_copyHint,0);
        auto *body = new HBox();
        _root->addWidget(body, 0);
        _preview = new MaterialPreview(_document);
        body->addWidget(_preview, 0);
        _shape->setOnSelectionChanged([this](int n) {
            _preview->setShape(n);
            _host.requestRepaint();
        });
        _reset->setOnClicked([this] {
            _preview->resetCamera();
            _host.requestRepaint();
        });
        auto *scroll = new ScrollView();
        body->addWidget(scroll, 440);
        auto *inspector = new VBox();
        inspector->setSpacing(4);
        scroll->setContentOwned(inspector);
        label(*inspector, L"Base color (linear RGBA, 0..1)");
        _color = fields(*inspector, {"material_r", "material_g", "material_b", "material_a"},
                        {L"R", L"G", L"B", L"A"});
        label(*inspector, L"Surface factors (0..1)");
        _factors = fields(*inspector, {"material_metallic", "material_roughness", "material_ao"},
                          {L"Metal", L"Rough", L"AO"});
        label(*inspector, L"Emission multiplier (linear RGB, 0..64)");
        _emission = fields(*inspector, {"material_er", "material_eg", "material_eb"}, {L"R", L"G", L"B"});
        label(*inspector, L"UV repeat density (.01..128)");
        _uvScale = fields(*inspector, {"material_uv_x", "material_uv_y"}, {L"U", L"V"});
        label(*inspector, L"UV offset (-1024..1024)");
        _uvOffset = fields(*inspector, {"material_offset_x", "material_offset_y"}, {L"U", L"V"});
        label(*inspector, L"Normal strength (0..4), roughness bias (-1..1)");
        _surface = fields(*inspector, {"material_normal", "material_roughness_bias"}, {L"Normal", L"Bias"});
        label(*inspector, L"Opacity and alpha cutoff (0..1)");
        _opacity = fields(*inspector, {"material_opacity", "material_cutoff"}, {L"Opacity", L"Cutoff"});
        auto *surfaceRow = row(*inspector);
        _alpha = new ComboBox();
        _alpha->setId("material_alpha_mode");
        _alpha->setItems({L"Opaque", L"Mask", L"Blend"});
        surfaceRow->addWidget(_alpha, 180);
        _doubleSided = new CheckBox();
        _doubleSided->setId("material_double_sided");
        _doubleSided->setText(L"Double sided");
        surfaceRow->addWidget(_doubleSided, 180);
        _alpha->setOnSelectionChanged([this](int) {
            if (!_syncing)
                commit();
        });
        _doubleSided->setOnToggled([this](bool) {
            if (!_syncing)
                commit();
        });
        label(*inspector, L"Opacity source (multiply with base-color alpha)");
        _opacitySource = new ComboBox();
        _opacitySource->setId("material_opacity_source");
        _opacitySource->setItems({L"Base color alpha", L"Opacity texture red", L"Opacity texture alpha"});
        inspector->addWidget(_opacitySource, 28);
        _opacitySource->setOnSelectionChanged([this](int) {
            if (!_syncing)
                commit();
        });
        _apply = new Button();
        _apply->setId("material_apply");
        _apply->setText(L"Apply parameters");
        _apply->setOnClicked([this] { commit(); });
        inspector->addWidget(_apply, 30);
        label(*inspector, L"Textures: source paths or Imported/textures/...");
        for (size_t i = 0; i < kEditorMaterialTextureSlots.size(); ++i)
        {
            auto *r = row(*inspector,34);
            auto *title = new TextLabel();
            title->setFontSize(11);
            title->setText(decodeUtf8Text(kEditorMaterialTextureSlots[i]));
            r->addWidget(title, 135);
            auto *input = new TextInput();
            input->setId(std::string("material_texture_") + kEditorMaterialTextureSlots[i]);
            input->setPlaceholder(L"Empty uses fallback");
            input->setOnSubmit([this](const auto &) { commit(); });
            input->setOnFocusLostNotify([this] { commit(); });
            r->addWidget(input, 0);
            _textureFields.push_back(input);
            auto* browse=new Button();browse->setId(std::string("material_browse_")+kEditorMaterialTextureSlots[i]);
            browse->setText(_host.localizedText("ui.editor.material.choose_texture",L"Choose..."));
            browse->setOnClicked([this,i]{showTextureDialog(i);});r->addWidget(browse,72);
            _textureBrowse.push_back(browse);
        }
        label(*inspector, L"Textures and shader (preserved on save)");
        _references = new TextArea();
        _references->setId("material_references");
        _references->setReadOnly(true);
        inspector->addWidget(_references, 230);
        _status = new TextLabel();
        _status->setId("material_status");
        _root->addWidget(_status, 44);
    }
    void showCopyDialog()
    {
        namespace fs=std::filesystem;
        if(_closed||(!_document->readOnly()&&!commit()))return;
        auto* manager=_host.uiManager();if(!manager){_status->setText(L"This host has no material dialog surface");return;}
        std::string candidate;
        try{
            const auto stem=fs::u8path(_document->path()).stem().generic_u8string();
            const std::string name(reinterpret_cast<const char*>(stem.data()),stem.size());
            for(unsigned index=0;index<10000;++index){
                candidate="materials/"+name+"_Editable"+(index?"_"+std::to_string(index):"")+".mat";
                if(!fs::exists(_sourceRoot/fs::u8path(candidate))&&!fs::is_symlink(_sourceRoot/fs::u8path(candidate)))break;
                candidate.clear();
            }
            if(candidate.empty()){_status->setText(L"No unused material copy name is available");return;}
        }catch(const std::exception& error){_status->setText(decodeUtf8Text(error.what()));return;}
        _copyDialog=std::make_unique<ModalDialog>();auto* dialog=_copyDialog.get();
        dialog->setId("material_copy_dialog");dialog->setSize({620,225});
        dialog->setAcceptText(_host.localizedText("ui.editor.material.create_copy",L"Create editable copy..."));
        dialog->setRejectText(_host.localizedText("ui.editor.import.cancel",L"Cancel"));
        auto* body=new VBox();body->setSpacing(6);body->setSize({588,153});
        auto* label=new TextLabel();label->setWordWrap(true);
        label->setText(_host.localizedText("ui.editor.material.copy_destination",L"Destination relative to project source assets (.mat, new file only)"));
        body->addWidget(label,34);
        auto* path=new TextInput();path->setId("material_copy_path");path->setText(decodeUtf8Text(candidate));body->addWidget(path,30);
        auto* error=new TextLabel();error->setId("material_copy_error");error->setTextColor({1,.4f,.3f,1});error->setWordWrap(true);body->addWidget(error,56);
        dialog->setBodyContentOwned(body);
        dialog->setAcceptValidator([this,path,error]{
            const auto destination=encodeUtf8Text(path->getText());std::string reason;
            if(!_document->saveEditableCopy(destination,&reason)){error->setText(decodeUtf8Text(reason));return false;}
            const auto created=std::filesystem::absolute(_sourceRoot/std::filesystem::u8path(destination));
            const auto text=created.generic_u8string();EditorOpenRequest request;
            request.resourcePath=std::string(reinterpret_cast<const char*>(text.data()),text.size());request.resourceKey=request.resourcePath;
            request.assetType="Material";request.preferredEditorId=kEditorMaterialExtensionId;
            const bool opened=_host.requestOpenDocument(request,&reason);
            _host.setStatusText(_host.localizedText("ui.editor.material.copy_created",L"Material copy created: ")+decodeUtf8Text(request.resourcePath)
                +(opened?std::wstring{}:L" — "+decodeUtf8Text(reason)));
            _host.requestRepaint();return true;
        });
        dialog->fitAndCenterInViewport(manager->root()->getSize(),12);dialog->openModal();_host.requestRepaint();
    }
    void showTextureDialog(std::size_t slot)
    {
        if(_closed||_document->readOnly()||!commit())return;
        auto* manager=_host.uiManager();if(!manager)return;
        const auto assets=std::make_shared<std::vector<EditorTextureAssetChoice>>(_host.textureAssets());
        const auto filtered=std::make_shared<std::vector<std::size_t>>();
        _textureDialog=std::make_unique<ModalDialog>();auto* dialog=_textureDialog.get();
        dialog->setId("material_texture_dialog");dialog->setSize({760,530});
        dialog->setAcceptText(_host.localizedText("ui.editor.material.apply_texture",L"Apply texture"));
        dialog->setRejectText(_host.localizedText("ui.editor.import.cancel",L"Cancel"));
        auto* body=new VBox();body->setSpacing(5);body->setSize({728,458});
        auto* search=new TextInput();search->setId("material_texture_search");
        search->setPlaceholder(_host.localizedText("ui.editor.material.texture_search",L"Search source and imported textures"));body->addWidget(search,30);
        auto* list=new ListView();list->setId("material_texture_choices");body->addWidget(list,350);
        auto* error=new TextLabel();error->setId("material_texture_error");error->setTextColor({1,.4f,.3f,1});error->setWordWrap(true);body->addWidget(error,50);
        const auto clear=_host.localizedText("ui.editor.material.texture_clear",L"Clear slot (use fallback)");
        const auto current=_document->textureAssetPath(slot);
        const auto currentLabel=decodeUtf8Text(current.empty()?std::string{}:
            (current.rfind("Imported/",0)==0?current:"Assets/"+current));
        const auto rebuild=[assets,filtered,list,clear,currentLabel](const std::wstring& query){
            auto folded=query;std::transform(folded.begin(),folded.end(),folded.begin(),
                [](wchar_t c){return c>=L'A'&&c<=L'Z'?wchar_t(c+L'a'-L'A'):c;});
            std::vector<std::wstring> labels{clear};filtered->clear();int selected=currentLabel.empty()?0:-1;
            for(std::size_t i=0;i<assets->size();++i){
                auto label=decodeUtf8Text((*assets)[i].displayPath),key=label;
                std::transform(key.begin(),key.end(),key.begin(),[](wchar_t c){return c>=L'A'&&c<=L'Z'?wchar_t(c+L'a'-L'A'):c;});
                if(!folded.empty()&&key.find(folded)==std::wstring::npos)continue;
                filtered->push_back(i);if(label==currentLabel)selected=static_cast<int>(labels.size());
                labels.push_back(std::move(label));
            }
            list->setItems(labels);list->setSelectedIndex(selected);
        };
        search->setOnTextChanged(rebuild);rebuild({});dialog->setBodyContentOwned(body);
        dialog->setAcceptValidator([this,slot,list,filtered,assets,error]{
            const int selected=list->getSelectedIndex();
            if(selected<0||std::size_t(selected)>filtered->size()){
                error->setText(_host.localizedText("ui.editor.material.texture_select",L"Select a texture or clear the slot."));return false;
            }
            const auto path=selected==0?std::string{}:(*assets)[(*filtered)[std::size_t(selected-1)]].absolutePath;
            std::string reason;if(!_document->setTextureAsset(slot,path,&reason)){error->setText(decodeUtf8Text(reason));return false;}
            refresh();_host.requestRepaint();return true;
        });
        dialog->fitAndCenterInViewport(manager->root()->getSize(),12);dialog->openModal();_host.requestRepaint();
    }
    bool commit()
    {
        if (_syncing || _closed)
            return false;
        if (_stamp != _document->revision())
        {
            refresh();
            _status->setText(L"Material changed; fields refreshed");
            return false;
        }
        auto s = _document->settings();
        std::vector<float> values;
        const auto read = [&](NumericFields *f, size_t count) { return f->readValues(count, values); };
        if (!read(_color, 4))
            return invalid();
        std::copy(values.begin(), values.end(), s.baseColor.begin());
        if (!read(_factors, 3))
            return invalid();
        s.metallic = values[0];
        s.roughness = values[1];
        s.ao = values[2];
        if (!read(_emission, 3))
            return invalid();
        std::copy(values.begin(), values.end(), s.emissive.begin());
        if (!read(_uvScale, 2))
            return invalid();
        s.surface.uvScaleX = values[0];
        s.surface.uvScaleY = values[1];
        if (!read(_uvOffset, 2))
            return invalid();
        s.surface.uvOffsetX = values[0];
        s.surface.uvOffsetY = values[1];
        if (!read(_surface, 2))
            return invalid();
        s.surface.normalStrength = values[0];
        s.surface.roughnessBias = values[1];
        if (!read(_opacity, 2))
            return invalid();
        s.opacity = values[0];
        s.alphaCutoff = values[1];
        s.alphaMode = uint8_t(_alpha->getSelectedIndex());
        s.opacitySource = uint8_t(_opacitySource->getSelectedIndex());
        s.doubleSided = _doubleSided->isChecked();
        std::string error;
        for (size_t i = 0; i < _textureFields.size(); ++i)
        {
            const auto requested = encodeUtf8Text(_textureFields[i]->getText());
            if (requested == _textureDisplay[i])
                continue;
            if (!_document->textureReferenceForAsset(requested, s.textures[i], &error))
            {
                _status->setText(decodeUtf8Text(error));
                return false;
            }
        }
        if (!_document->replaceSettings(s, &error))
        {
            _status->setText(decodeUtf8Text(error));
            return false;
        }
        refresh();
        _host.requestRepaint();
        return true;
    }
    bool invalid()
    {
        _status->setText(L"Enter complete finite numbers before applying");
        return false;
    }
    std::vector<std::vector<float>> numericValues() const
    {
        const auto &s = _document->settings();
        return {{s.baseColor.begin(), s.baseColor.end()},
                {s.metallic, s.roughness, s.ao},
                {s.emissive.begin(), s.emissive.end()},
                {s.surface.uvScaleX, s.surface.uvScaleY},
                {s.surface.uvOffsetX, s.surface.uvOffsetY},
                {s.surface.normalStrength, s.surface.roughnessBias},
                {s.opacity, s.alphaCutoff}};
    }
    bool formDirty() const
    {
        if (!_alpha || !_doubleSided || !_opacitySource)
            return false;
        const auto values = numericValues();
        for (size_t row = 0; row < _fields.size(); ++row)
            for (size_t c = 0; c < values[row].size(); ++c)
                if (_fields[row]->inputs()[c]->getText() != formatNumber(values[row][c], 9))
                    return true;
        for (size_t i = 0; i < _textureFields.size(); ++i)
            if (encodeUtf8Text(_textureFields[i]->getText()) != _textureDisplay[i])
                return true;
        return _alpha->getSelectedIndex() != _document->settings().alphaMode ||
               _opacitySource->getSelectedIndex() != _document->settings().opacitySource ||
               _doubleSided->isChecked() != _document->settings().doubleSided;
    }
    void refresh()
    {
        _syncing = true;
        const auto &s = _document->settings();
        const bool ro = _document->readOnly();
        _color->setValues({s.baseColor.begin(), s.baseColor.end()}, ro);
        _factors->setValues({s.metallic, s.roughness, s.ao}, ro);
        _emission->setValues({s.emissive.begin(), s.emissive.end()}, ro);
        _uvScale->setValues({s.surface.uvScaleX, s.surface.uvScaleY}, ro);
        _uvOffset->setValues({s.surface.uvOffsetX, s.surface.uvOffsetY}, ro);
        _surface->setValues({s.surface.normalStrength, s.surface.roughnessBias}, ro);
        _opacity->setValues({s.opacity, s.alphaCutoff}, ro);
        _alpha->setSelectedIndex(s.alphaMode);
        _opacitySource->setSelectedIndex(s.opacitySource);
        _doubleSided->setChecked(s.doubleSided);
        // Nine significant digits round-trip float32. Merely submitting an
        // untouched field must never quantize authored high-precision values.
        const auto exact = numericValues();
        for (size_t row = 0; row < _fields.size(); ++row)
            for (size_t c = 0; c < exact[row].size(); ++c)
                _fields[row]->inputs()[c]->setText(formatNumber(exact[row][c], 9));
        _alpha->setEnabled(!ro);
        _opacitySource->setEnabled(!ro);
        _doubleSided->setEnabled(!ro);
        _apply->setEnabled(!ro);
        for (size_t i = 0; i < _textureFields.size(); ++i)
        {
            _textureDisplay[i] = _document->textureAssetPath(i);
            _textureFields[i]->setReadOnly(ro);
            _textureBrowse[i]->setEnabled(!ro);
            _textureFields[i]->setText(decodeUtf8Text(_textureDisplay[i]));
        }
        std::string refs = "Shader: " + _document->shaderReference();
        for (const auto &[slot, path] : _document->textureReferences())
            refs += "\n" + slot + ": " + path;
        _references->setText(decodeUtf8Text(refs));
        _status->setText(ro ? L"Read-only imported/external material. Independent studio preview."
                            : L"Drag to orbit, wheel to zoom. Studio light / ACES / Gamma 2.2. Save updates "
                              L"the material file.");
        _stamp = _document->revision();
        _commands.refresh();
        _preview->markDirty();
        _syncing = false;
    }
    std::shared_ptr<EditorMaterialDocument> _document;
    IEditorHostServices &_host;
    EditorCommandButtons _commands;
    VBox *_root = nullptr;
    MaterialPreview *_preview = nullptr;
    NumericFields *_color = nullptr, *_factors = nullptr, *_emission = nullptr, *_uvScale = nullptr,
                  *_uvOffset = nullptr, *_surface = nullptr, *_opacity = nullptr;
    std::vector<NumericFields *> _fields;
    std::vector<TextInput *> _textureFields;
    std::array<std::string, 7> _textureDisplay;
    ComboBox *_alpha = nullptr, *_shape = nullptr, *_opacitySource = nullptr;
    CheckBox *_doubleSided = nullptr;
    Button *_apply = nullptr, *_reset = nullptr, *_copy = nullptr;
    std::vector<Button*> _textureBrowse;
    std::unique_ptr<ModalDialog> _copyDialog,_textureDialog;
    std::filesystem::path _sourceRoot;
    TextLabel *_status = nullptr, *_copyHint = nullptr;
    TextArea *_references = nullptr;
    uint64_t _stamp = UINT64_MAX;
    bool _closed = false, _syncing = false;
};
} // namespace
EditorDescriptor makeEditorMaterialDescriptor()
{
    EditorDescriptor d;
    d.id = kEditorMaterialExtensionId;
    d.displayName = L"PBR Material";
    d.priority = 100;
    d.extensions = {".mat", ".aymat"};
    d.assetTypes = {"Material", "runtime.material"};
    d.createDocument = [](const EditorOpenRequest &r,
                          std::string &error) -> std::shared_ptr<IEditorDocument> {
        auto doc = std::make_shared<EditorMaterialDocument>();
        return doc->initialize(r, error) ? doc : nullptr;
    };
    d.createView = [](const std::shared_ptr<IEditorDocument> &d,
                      IEditorHostServices &host) -> std::unique_ptr<IEditorView> {
        auto doc = std::dynamic_pointer_cast<EditorMaterialDocument>(d);
        return doc ? std::make_unique<MaterialView>(doc, host) : nullptr;
    };
    return d;
}
bool registerEditorMaterialExtension(EditorExtensionRegistry &registry, std::string *error)
{
    return registry.registerEditor(makeEditorMaterialDescriptor(), error);
}
} // namespace ayt::editor
