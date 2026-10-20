// Opt-in real-device acceptance of the actual Material Dock view.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <AYEditor/EditorMaterialDocument.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYRenderer.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYUI/Widget.h>
#include <AYUI/UIManager.h>
#include <AYUI/ComboBox.h>
#include <AYIO/File.h>
#include "detail/TextureImageLoader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
namespace fs = std::filesystem;
using namespace ayt;
std::string utf8(const fs::path &p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
void require(bool condition, const char *text)
{
    if (!condition)
        throw std::runtime_error(text);
}
ui::Widget *find(ui::Widget *root, const std::string &id)
{
    if (root->getId() == id)
        return root;
    for (auto *child : root->getChildren())
        if (auto *found = find(child, id))
            return found;
    return nullptr;
}
struct Host : editor::IEditorHostServices
{
    editor::EditorWorkspace value;
    std::string root;
    editor::EditorWorkspace &workspace() noexcept override
    {
        return value;
    }
    const std::string &projectRoot() const noexcept override
    {
        return root;
    }
    void requestRepaint() override
    {
    }
    void setStatusText(const std::wstring &) override
    {
    }
};
struct Fixture
{
    HWND window = nullptr;
    render::Renderer renderer;
    render::UIRenderBackend gpu;
    ui::UIManager manager;
    Host host;
    std::shared_ptr<editor::EditorMaterialDocument> document =
        std::make_shared<editor::EditorMaterialDocument>();
    std::unique_ptr<editor::IEditorView> view;
    fs::path output;
    explicit Fixture(fs::path target, bool d3d12) : output(fs::absolute(target))
    {
        const auto assets = output / "project" / "Assets";
        fs::create_directories(assets / "textures");
        host.root = utf8(assets.parent_path());
        const fs::path source = AY_MATERIAL_LIBRARY_HINT;
        fs::copy_file(source / "pbr.phoskia", assets / "pbr.phoskia", fs::copy_options::overwrite_existing);
        for (const char *suffix : {"d", "n", "r", "m", "ao", "e"})
            fs::copy_file(source / "textures" / (std::string("ceramic_") + suffix + ".png"),
                          assets / "textures" / (std::string("ceramic_") + suffix + ".png"),
                          fs::copy_options::overwrite_existing);
        const auto path = assets / fs::u8path("瓷砖.mat");
        fs::copy_file(source / "ceramic.mat", path, fs::copy_options::overwrite_existing);
        std::string error;
        require(document->initialize({utf8(path)}, error), error.c_str());
        window = CreateWindowExW(0, L"STATIC", L"Material authoring view", WS_OVERLAPPED, 0, 0, 1050, 680,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "hidden render window");
        render::InitDesc init;
        init.windowHandle = window;
        init.width = 1050;
        init.height = 680;
        init.vsync = false;
        init.msaa = 0;
        init.backend = d3d12 ? render::Backend::Direct3D12 : render::Backend::Direct3D11;
        require(renderer.initialize(init), "renderer initialize");
        require(gpu.initialize(renderer), "UI renderer initialize");
        gpu.setFramebufferSize(1050, 680);
        manager.initialize(&gpu);
        manager.setClientSize(1050, 680);
        view = editor::makeEditorMaterialDescriptor().createView(document, host);
        require(bool(view), "actual material Dock view");
        view->rootWidget()->setSize({1050, 680});
        view->rootWidget()->performLayout();
    }
    ~Fixture()
    {
        if (view)
            view->prepareForUiShutdown();
        view.reset();
        manager.shutdown();
        gpu.shutdown();
        renderer.shutdown();
        if (window)
            DestroyWindow(window);
    }
    void frame(const std::string &name = {})
    {
        renderer.beginCompositeFrame({}, 1050, 680);
        gpu.beginFrame();
        gpu.beginCanvas({0, 0, 1050, 680});
        view->tick(1.f / 60);
        view->rootWidget()->render(gpu);
        gpu.endCanvas();
        gpu.endFrame();
        if (!name.empty())
            require(renderer.captureScreenshot(utf8(output / name)), "capture material authoring view");
        renderer.endFrame();
    }
    auto capture(const char *name)
    {
        for (int i = 0; i < 8; ++i)
            frame();
        frame(name);
        for (int i = 0; i < 8; ++i)
            frame();
        auto image = render::detail::decodeImageFile(utf8(output / name));
        require(image.isValid(), "captured material pixels");
        require(image.width == 1050 && image.height == 680 && image.rgba8.size() == 1050u * 680u * 4u,
                "Material capture dimensions match the pixel comparison fixture");
        return image;
    }
};
} // namespace
int main(int argc, char **argv)
{
    try
    {
        Fixture f(argc > 1 ? argv[1] : "out/material-preview-render",
                  argc > 2 && std::string(argv[2]) == "d3d12");
        const auto camera = f.renderer.mainCameraPosition();
        const auto original = f.document->settings();
        const auto clean = f.capture("material-open.png");
        require(!f.document->isDirty(), "opening/view rendering retains clean document");
        auto edited = original;
        edited.baseColor = {.9f, .35f, .12f, 1};
        edited.surface.uvScaleX = 2.7f;
        edited.surface.uvScaleY = 1.7f;
        edited.surface.normalStrength = 2;
        edited.surface.roughnessBias = .25f;
        std::string error;
        require(f.document->replaceSettings(edited, &error), error.c_str());
        const auto changed = f.capture("material-edited.png");
        size_t different = 0, bright = 0;
        for (int y = 90; y < 580; ++y)
            for (int x = 40; x < 570; ++x)
            {
                const auto p = (y * 1050 + x) * 4;
                if (std::abs(int(clean.rgba8[p]) - int(changed.rgba8[p])) > 8)
                    ++different;
                if (clean.rgba8[p] > 50 || clean.rgba8[p + 1] > 70 || clean.rgba8[p + 2] > 70)
                    ++bright;
            }
        std::cout << "[Material Dock GPU] visible=" << bright << " changed=" << different << '\n';
        require(bright > 1000, "actual PBR surface visible in isolated preview");
        require(different > 1000, "unsaved edits visibly update actual preview");
        auto *shape = dynamic_cast<ui::ComboBox *>(find(f.view->rootWidget(), "material_shape"));
        require(shape != nullptr, "preview shape selector is available");
        for (int selection : {1, 2})
        {
            shape->setSelectedIndexAndNotify(selection);
            const auto shaped =
                f.capture(selection == 1 ? "material-scaled-sphere.png" : "material-uv-tile.png");
            size_t changes = 0;
            for (int y = 90; y < 580; ++y)
                for (int x = 40; x < 570; ++x)
                {
                    const auto p = (y * 1050 + x) * 4;
                    if (std::abs(int(shaped.rgba8[p]) - int(changed.rgba8[p])) > 8)
                        ++changes;
                }
            require(changes > 1000, "shape selection changes actual PBR geometry");
            require(f.document->settings() == edited, "shape selection preserves authored material values");
        }
        shape->setSelectedIndexAndNotify(0);
        require(f.document->save(&error), error.c_str());
        require(!f.document->isDirty(), "save commits material");
        editor::EditorMaterialDocument reopened;
        require(reopened.initialize({f.document->path()}, error), error.c_str());
        require(reopened.settings() == edited, "existing material codec saves authored fields");
        require(f.document->undo(), "material undo after save");
        const auto undo = f.capture("material-undo.png");
        size_t residual = 0;
        for (int y = 90; y < 580; ++y)
            for (int x = 40; x < 570; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    const auto p = (y * 1050 + x) * 4 + c;
                    if (std::abs(int(clean.rgba8[p]) - int(undo.rgba8[p])) > 1)
                        ++residual;
                }
        require(residual == 0, "undo restores preview pixels within RGBA8 quantization");
        const auto after = f.renderer.mainCameraPosition();
        require(after.x == camera.x && after.y == camera.y && after.z == camera.z,
                "authoring preview preserves main camera");
        std::cout << "PASS material Dock actual GPU preview: visible texture, unsaved update, save/reopen, "
                     "undo within RGBA8 quantization, isolated camera\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
