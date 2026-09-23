#include "AYEditorNewProjectController.h"

#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>

#include <filesystem>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

std::string narrow(const std::wstring& text)
{
    if (text.empty()) return {};
#if defined(_WIN32)
    const int size = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<std::size_t>(size), '\0');
    (void)::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        output.data(), size, nullptr, nullptr);
    return output;
#else
    std::string output;
    for (wchar_t character : text) {
        output.push_back(character >= 0 && character <= 0x7f
            ? static_cast<char>(character) : '?');
    }
    return output;
#endif
}

std::string diagnosticsText(const std::vector<std::string>& diagnostics)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < diagnostics.size(); ++index) {
        if (index != 0u) output << '\n';
        output << diagnostics[index];
    }
    return output.str();
}

template <typename T>
T* widgetAs(ayt::ui::UIManager& ui, const char* id)
{
    return dynamic_cast<T*>(ui.findById(id));
}

} // namespace

EditorNewProjectResult createEditorGameProject(
    const EditorNewProjectRequest& request)
{
    EditorNewProjectResult result;
    if (request.parentDirectory.empty()) {
        result.error = "Choose a parent folder for the new project.";
        return result;
    }

    const std::string projectId =
        ayt::project::suggestGameProjectId(request.displayName);
    const fs::path destination =
        fs::u8path(request.parentDirectory) / fs::u8path(projectId);
    const ayt::project::GameProjectScaffoldPlan plan =
        ayt::project::planGameProjectScaffold({
            .destination = destination.string(),
            .engineSource = request.engineSourceRoot,
            .displayName = request.displayName,
            .projectId = projectId,
            .profile = request.profile,
        });
    if (!plan) {
        result.error = diagnosticsText(plan.diagnostics);
        return result;
    }

    if (!ayt::project::writeGameProjectScaffold(plan, &result.error)) {
        return result;
    }
    result.success = true;
    result.projectRoot = plan.projectRoot;
    return result;
}

struct EditorNewProjectController::Impl {
    explicit Impl(EditorNewProjectConfig value) : config(std::move(value)) {}

    std::wstring localized(std::string_view key,
                           std::wstring_view fallback) const
    {
        return config.localize
            ? config.localize(key, fallback) : std::wstring(fallback);
    }

    void refreshDestination()
    {
        if (status == nullptr || name == nullptr || parent == nullptr) return;
        const std::string projectId = ayt::project::suggestGameProjectId(
            narrow(name->getText()));
        const fs::path destination = fs::u8path(narrow(parent->getText()))
            / fs::u8path(projectId);
        status->setText(localized("ui.editor.new_project.destination",
                                  L"Project folder: ")
            + ayt::ui::decodeUtf8Text(destination.string()));
    }

    bool attach(ayt::ui::UIManager& manager, std::string* error)
    {
        ui = &manager;
        name = widgetAs<ayt::ui::TextInput>(manager, "new_project_name");
        parent = widgetAs<ayt::ui::TextInput>(manager, "new_project_parent");
        profile = widgetAs<ayt::ui::ComboBox>(manager, "new_project_profile");
        status = widgetAs<ayt::ui::TextLabel>(manager, "new_project_status");
        auto* browse = widgetAs<ayt::ui::Button>(
            manager, "new_project_browse");
        auto* create = widgetAs<ayt::ui::Button>(
            manager, "new_project_create");
        if (name == nullptr || parent == nullptr || profile == nullptr
            || status == nullptr || browse == nullptr || create == nullptr) {
            if (error != nullptr) {
                *error = "New Project layout is missing required controls.";
            }
            detach();
            return false;
        }

        name->setText(L"My Game");
        parent->setText(ayt::ui::decodeUtf8Text(
            config.initialParentDirectory));
        profile->setItems({
            localized("ui.editor.new_project.profile_2d", L"2D Game"),
            localized("ui.editor.new_project.profile_3d", L"3D Game")});
        profile->setSelectedIndex(1);
        name->setOnTextChanged([this](const std::wstring&) {
            refreshDestination();
        });
        parent->setOnTextChanged([this](const std::wstring&) {
            refreshDestination();
        });
        browse->setOnClicked([this]() {
            if (!config.chooseParentDirectory) return;
            const std::string selected = config.chooseParentDirectory();
            if (!selected.empty() && parent != nullptr) {
                parent->setText(ayt::ui::decodeUtf8Text(selected));
                refreshDestination();
            }
        });
        create->setOnClicked([this]() {
            std::string ignored;
            (void)createAndOpen(&ignored);
        });
        refreshDestination();
        return true;
    }

    void detach()
    {
        ui = nullptr;
        name = nullptr;
        parent = nullptr;
        profile = nullptr;
        status = nullptr;
    }

    bool createAndOpen(std::string* error)
    {
        if (name == nullptr || parent == nullptr || profile == nullptr) {
            if (error != nullptr) *error = "New Project window is not attached.";
            return false;
        }
        EditorNewProjectRequest request;
        request.engineSourceRoot = config.engineSourceRoot;
        request.parentDirectory = narrow(parent->getText());
        request.displayName = narrow(name->getText());
        request.profile = profile->getSelectedIndex() == 0
            ? ayt::project::GameProjectTemplateProfile::Client2D
            : ayt::project::GameProjectTemplateProfile::Client3D;
        const EditorNewProjectResult created =
            createEditorGameProject(request);
        if (!created.success) {
            if (status != nullptr) {
                status->setText(ayt::ui::decodeUtf8Text(created.error));
            }
            if (error != nullptr) *error = created.error;
            return false;
        }

        std::string openError;
        if (!config.openProject
            || !config.openProject(created.projectRoot, &openError)) {
            const std::string message = openError.empty()
                ? "Project was created, but the Editor could not open it."
                : "Project was created, but the Editor could not open it: "
                    + openError;
            if (status != nullptr) {
                status->setText(ayt::ui::decodeUtf8Text(message));
            }
            if (error != nullptr) *error = message;
            return false;
        }
        if (status != nullptr) {
            status->setText(localized("ui.editor.new_project.opening",
                L"Project created. Opening it now..."));
        }
        return true;
    }

    EditorNewProjectConfig config;
    ayt::ui::UIManager* ui = nullptr;
    ayt::ui::TextInput* name = nullptr;
    ayt::ui::TextInput* parent = nullptr;
    ayt::ui::ComboBox* profile = nullptr;
    ayt::ui::TextLabel* status = nullptr;
};

EditorNewProjectController::EditorNewProjectController(
    EditorNewProjectConfig config)
    : _impl(std::make_unique<Impl>(std::move(config)))
{
}

EditorNewProjectController::~EditorNewProjectController() = default;

bool EditorNewProjectController::attach(
    ayt::ui::UIManager& ui, std::string* error)
{
    return _impl != nullptr && _impl->attach(ui, error);
}

void EditorNewProjectController::detach()
{
    if (_impl != nullptr) _impl->detach();
}

bool EditorNewProjectController::createAndOpen(std::string* error)
{
    return _impl != nullptr && _impl->createAndOpen(error);
}

} // namespace ayt::editor
