#include "AYEditorNewProjectController.h"

#include <AYTest.h>
#include <AYUI/ComboBox.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

namespace fs = std::filesystem;
using namespace ayt::editor;
using ayt::project::GameProjectTemplateProfile;

class NewProjectSandbox {
public:
    NewProjectSandbox()
    {
        root = fs::temp_directory_path()
            / ("ayeditor-new-project-" + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
        engine = root / "AliyatEngine";
        parent = root / "Games";
        fs::create_directories(engine / "cmake");
        std::ofstream helper(engine / "cmake/AYGameApplication.cmake");
        helper << "# test helper\n";
    }

    ~NewProjectSandbox()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
    fs::path engine;
    fs::path parent;
};

std::string readText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_SUITE(AYEditor_NewProject)

TEST_CASE(editor_adapter_uses_shared_identity_and_2d_template)
{
    NewProjectSandbox sandbox;
    const EditorNewProjectResult created = createEditorGameProject({
        .parentDirectory = sandbox.parent.string(),
        .engineSourceRoot = sandbox.engine.string(),
        .displayName = "First Editor Game",
        .profile = GameProjectTemplateProfile::Client2D,
    });

    CHECK(created.success);
    CHECK(created.error.empty());
    CHECK(fs::path(created.projectRoot).filename() == "first-editor-game");
    CHECK(fs::is_regular_file(
        fs::path(created.projectRoot) / "project.ayproject.json"));
    CHECK(readText(fs::path(created.projectRoot) / "CMakeLists.txt")
        .find("PROFILE CLIENT_2D") != std::string::npos);

    const EditorNewProjectResult duplicate = createEditorGameProject({
        .parentDirectory = sandbox.parent.string(),
        .engineSourceRoot = sandbox.engine.string(),
        .displayName = "First Editor Game",
    });
    CHECK_FALSE(duplicate.success);
    CHECK(duplicate.error.find("already exists") != std::string::npos);
}

TEST_CASE(new_project_layout_loads_and_controller_localizes_profile_choices)
{
    const fs::path layout = fs::path(AY_EDITOR_TEST_SOURCE_DIR)
        / "ui/new_project.ui.json";
    CHECK(fs::is_regular_file(layout));

    ayt::ui::MockRenderer renderer;
    ayt::ui::UIManager ui;
    ui.initialize(&renderer);
    ui.setClientSize(760.0f, 500.0f);
    CHECK(ui.loadLayout(layout.string()));

    EditorNewProjectConfig config;
    config.initialParentDirectory = fs::temp_directory_path().string();
    config.localize = [](std::string_view key, std::wstring_view fallback) {
        if (key == "ui.editor.new_project.profile_2d") return std::wstring(L"二维游戏");
        if (key == "ui.editor.new_project.profile_3d") return std::wstring(L"三维游戏");
        if (key == "ui.editor.new_project.destination") return std::wstring(L"项目文件夹：");
        return std::wstring(fallback);
    };
    EditorNewProjectController controller(std::move(config));
    std::string error;
    CHECK(controller.attach(ui, &error));
    CHECK(error.empty());
    ui.layout();

    auto* profile = dynamic_cast<ayt::ui::ComboBox*>(
        ui.findById("new_project_profile"));
    auto* status = dynamic_cast<ayt::ui::TextLabel*>(
        ui.findById("new_project_status"));
    CHECK(profile != nullptr && profile->getItemCount() == 2u);
    CHECK(profile != nullptr && profile->getItem(0) == L"二维游戏");
    CHECK(status != nullptr
        && status->getText().find(L"项目文件夹：") == 0u);

    controller.detach();
    ui.shutdown();
}

TEST_SUITE_END
