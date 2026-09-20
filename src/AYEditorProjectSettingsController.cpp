#include "AYEditorProjectSettingsController.h"

#include "AYEditor/EditorProjectDescriptor.h"

#include <AYResource/ProjectBuild.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ListView.h>
#include <AYUI/ProgressBar.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iterator>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

template <typename T>
T* widgetAs(ayt::ui::UIManager& ui, const char* id)
{
    return dynamic_cast<T*>(ui.findById(id));
}

void appendUtf8(std::string& output, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

std::string narrow(const std::wstring& text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu
                && index + 1 < text.size()) {
                const auto low = static_cast<std::uint32_t>(text[index + 1]);
                if (low >= 0xdc00u && low <= 0xdfffu) {
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u)
                        + (low - 0xdc00u);
                    ++index;
                }
            }
        }
        if (codePoint >= 0xd800u && codePoint <= 0xdfffu) codePoint = 0xfffdu;
        appendUtf8(output, (std::min)(codePoint, std::uint32_t{0x10ffffu}));
    }
    return output;
}

std::wstring wide(const std::string& text)
{
    return ayt::ui::decodeUtf8Text(text);
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> values;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) values.push_back(std::move(line));
    }
    return values;
}

std::string joinLines(const std::vector<std::string>& values)
{
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0u) result.push_back('\n');
        result += values[index];
    }
    return result;
}

std::string relativePortable(const fs::path& base, const std::string& selected)
{
    if (selected.empty()) return {};
    std::error_code error;
    fs::path value = fs::u8path(selected);
    if (!value.is_absolute()) return value.lexically_normal().generic_string();
    const fs::path absoluteBase = fs::absolute(base, error).lexically_normal();
    if (error) return value.lexically_normal().generic_string();
    const fs::path relative = value.lexically_normal().lexically_relative(
        absoluteBase);
    if (relative.empty() || relative.is_absolute()
        || *relative.begin() == "..") {
        return value.lexically_normal().generic_string();
    }
    return relative.generic_string();
}

ayt::resource::ProjectBuildProfile defaultProfile(
    const std::string& projectId)
{
    ayt::resource::ProjectBuildProfile profile;
    profile.schemaVersion = 1u;
    profile.id = projectId.empty() ? "windows-development"
                                   : projectId + "-windows-development";
    profile.platform = "windows";
    profile.architecture = "x64";
    profile.configuration = "Development";
    profile.code.enabled = true;
    profile.code.backend = "cmake";
    profile.code.cmakeExecutable = "cmake";
    profile.code.sourceDirectory = ".";
    profile.code.configurePreset = "windows-client-debug";
    profile.code.buildPreset = "windows-client-debug";
    profile.code.target = projectId.empty() ? "Game" : projectId;
    profile.code.artifact = "out/build/windows-client-debug/"
        + (projectId.empty() ? std::string("Game") : projectId) + ".exe";
    profile.content.assetRoot = "Assets";
    profile.content.outputSubdirectory = "Content";
    profile.content.defaultTransform =
        ayt::resource::ProjectAssetTransform::Raw;
    profile.content.defaultStorage =
        ayt::resource::ProjectAssetStorage::Loose;
    profile.content.rules = {
        {"Developer/**", ayt::resource::ProjectAssetTransform::Exclude,
         ayt::resource::ProjectAssetStorage::Loose, "core", false},
        {"**/*.gameflow.json", ayt::resource::ProjectAssetTransform::Raw,
         ayt::resource::ProjectAssetStorage::Loose, "core", false},
        {"**/*.uiflow.json", ayt::resource::ProjectAssetTransform::Raw,
         ayt::resource::ProjectAssetStorage::Loose, "core", false},
        {"**/*.ui.json", ayt::resource::ProjectAssetTransform::Raw,
         ayt::resource::ProjectAssetStorage::Loose, "core", false},
        {"**/*.ayscene", ayt::resource::ProjectAssetTransform::Raw,
         ayt::resource::ProjectAssetStorage::Loose, "core", false},
    };
    profile.cache.enabled = true;
    profile.cache.root = ".cookCache";
    profile.cache.policy = ayt::resource::ProjectCookPolicy::Auto;
    profile.package.output = "out/package/windows-development";
    profile.package.compression = "zstd";
    profile.package.atomic = true;
    profile.run.workingDirectory = ".";
    return profile;
}

std::string transformText(ayt::resource::ProjectAssetTransform value)
{
    switch (value) {
    case ayt::resource::ProjectAssetTransform::Auto: return "Auto";
    case ayt::resource::ProjectAssetTransform::Raw: return "Raw";
    case ayt::resource::ProjectAssetTransform::Cook: return "Cook";
    case ayt::resource::ProjectAssetTransform::Exclude: return "Exclude";
    }
    return "Auto";
}

ayt::resource::ProjectAssetTransform transformAt(int index)
{
    switch (index) {
    case 1: return ayt::resource::ProjectAssetTransform::Raw;
    case 2: return ayt::resource::ProjectAssetTransform::Cook;
    case 3: return ayt::resource::ProjectAssetTransform::Exclude;
    default: return ayt::resource::ProjectAssetTransform::Auto;
    }
}

int transformIndex(ayt::resource::ProjectAssetTransform value)
{
    switch (value) {
    case ayt::resource::ProjectAssetTransform::Auto: return 0;
    case ayt::resource::ProjectAssetTransform::Raw: return 1;
    case ayt::resource::ProjectAssetTransform::Cook: return 2;
    case ayt::resource::ProjectAssetTransform::Exclude: return 3;
    }
    return 0;
}

ayt::resource::ProjectCookPolicy policyAt(int index)
{
    switch (index) {
    case 1: return ayt::resource::ProjectCookPolicy::CacheOnly;
    case 2: return ayt::resource::ProjectCookPolicy::Force;
    case 3: return ayt::resource::ProjectCookPolicy::Never;
    default: return ayt::resource::ProjectCookPolicy::Auto;
    }
}

int policyIndex(ayt::resource::ProjectCookPolicy value)
{
    switch (value) {
    case ayt::resource::ProjectCookPolicy::Auto: return 0;
    case ayt::resource::ProjectCookPolicy::CacheOnly: return 1;
    case ayt::resource::ProjectCookPolicy::Force: return 2;
    case ayt::resource::ProjectCookPolicy::Never: return 3;
    }
    return 0;
}

std::wstring ruleLabel(const ayt::resource::ProjectBuildRule& rule,
                       std::size_t index)
{
    const std::string storage = rule.storage
        == ayt::resource::ProjectAssetStorage::Pak
        ? "Pak:" + rule.chunk : "Loose";
    return std::to_wstring(index + 1u) + L". " + wide(rule.match)
        + L"   [" + wide(transformText(rule.transform)) + L" / "
        + wide(storage) + L"]";
}

} // namespace

struct EditorProjectSettingsController::Impl {
    struct BuildProfileEntry {
        std::string path;
        ayt::resource::ProjectBuildProfile profile;
        bool dirty = false;
    };

    struct AsyncState {
        std::mutex mutex;
        ayt::resource::ProjectBuildProgress progress;
        ayt::resource::ProjectBuildResult result;
        bool finished = false;
    };

    explicit Impl(EditorProjectSettingsConfig value)
        : config(std::move(value)) {}

    template <typename T>
    T* find(const char* id) const
    {
        return ui != nullptr ? widgetAs<T>(*ui, id) : nullptr;
    }

    std::string text(const char* id) const
    {
        if (const auto* input = find<ayt::ui::TextInput>(id)) {
            return narrow(input->getText());
        }
        if (const auto* area = find<ayt::ui::TextArea>(id)) {
            return narrow(area->getText());
        }
        return {};
    }

    void setText(const char* id, const std::string& value)
    {
        if (auto* input = find<ayt::ui::TextInput>(id)) {
            input->setText(wide(value));
        } else if (auto* area = find<ayt::ui::TextArea>(id)) {
            area->setText(wide(value));
        }
    }

    bool checked(const char* id) const
    {
        const auto* box = find<ayt::ui::CheckBox>(id);
        return box != nullptr && box->isChecked();
    }

    void setChecked(const char* id, bool value)
    {
        if (auto* box = find<ayt::ui::CheckBox>(id)) box->setChecked(value);
    }

    int selected(const char* id) const
    {
        const auto* combo = find<ayt::ui::ComboBox>(id);
        return combo != nullptr ? combo->getSelectedIndex() : -1;
    }

    void setSelected(const char* id, int value)
    {
        if (auto* combo = find<ayt::ui::ComboBox>(id)) {
            combo->setSelectedIndex(value);
        }
    }

    void statusText(const std::wstring& value, bool failure = false)
    {
        lastError = failure ? narrow(value) : std::string{};
        if (status != nullptr) status->setText(value);
    }

    void statusText(const std::string& value, bool failure = false)
    {
        statusText(wide(value), failure);
    }

    std::wstring localized(std::string_view key,
                           std::wstring_view fallback) const
    {
        return config.localize != nullptr
            ? config.localize(key, fallback) : std::wstring(fallback);
    }

    void bindButton(const char* id, std::function<void()> callback)
    {
        if (auto* button = find<ayt::ui::Button>(id)) {
            button->setOnClicked(std::move(callback));
        }
    }

    void markDirty()
    {
        if (!refreshing) dirty = true;
    }

    void bindDirtyInput(const char* id)
    {
        if (auto* input = find<ayt::ui::TextInput>(id)) {
            input->setOnTextChanged([this](const std::wstring&) {
                markDirty();
            });
        } else if (auto* area = find<ayt::ui::TextArea>(id)) {
            area->setOnTextChanged([this](const std::wstring&) {
                markDirty();
            });
        }
    }

    void bindDirtyCheck(const char* id)
    {
        if (auto* box = find<ayt::ui::CheckBox>(id)) {
            box->setOnToggled([this](bool) { markDirty(); });
        }
    }

    void bindDirtyCombo(const char* id)
    {
        if (auto* combo = find<ayt::ui::ComboBox>(id)) {
            combo->setOnSelectionChanged([this](int) { markDirty(); });
        }
    }

    bool load(std::string* error)
    {
        if (error != nullptr) error->clear();
        if (config.projectRoot.empty()) {
            if (error != nullptr) *error = "No project is open.";
            return false;
        }
        project = EditorProjectDescriptor::load(config.projectRoot, error);
        if (!project) {
            const fs::path descriptor = fs::path(config.projectRoot)
                / std::string(kEditorProjectDescriptorFile);
            std::error_code existsError;
            if (fs::is_regular_file(descriptor, existsError)) return false;
            project.schemaVersion = kEditorProjectDescriptorSchemaVersion;
            project.id = fs::path(config.projectRoot).filename().string();
            if (project.id.empty()) project.id = "game";
            project.displayName = project.id;
            project.assetRoot = "Assets";
            project.defaultSceneView = "Auto";
            project.run.workingDirectory = ".";
            project.sourcePath = descriptor.string();
            if (error != nullptr) error->clear();
        }

        profiles.clear();
        const fs::path profileRoot = fs::path(config.projectRoot)
            / "BuildProfiles";
        std::error_code scanError;
        if (fs::is_directory(profileRoot, scanError)) {
            for (fs::recursive_directory_iterator it(profileRoot, scanError), end;
                 !scanError && it != end; it.increment(scanError)) {
                if (!it->is_regular_file()) continue;
                const std::string name = it->path().filename().string();
                if (name.size() < 13u
                    || name.substr(name.size() - 13u) != ".aybuild.json") {
                    continue;
                }
                std::string profileError;
                auto profile = ayt::resource::ProjectBuildProfile::load(
                    it->path().string(), &profileError);
                if (!profile) {
                    if (error != nullptr) {
                        *error = "Invalid build profile " + name + ": "
                            + profileError;
                    }
                    return false;
                }
                profiles.push_back({it->path().string(), std::move(profile), false});
            }
        }
        std::sort(profiles.begin(), profiles.end(),
            [](const BuildProfileEntry& left, const BuildProfileEntry& right) {
                return left.path < right.path;
            });
        if (profiles.empty()) {
            ayt::resource::ProjectBuildProfile profile = defaultProfile(project.id);
            const std::string path = (profileRoot
                / "windows-development.aybuild.json").string();
            profile.sourcePath = path;
            profiles.push_back({path, std::move(profile), true});
        }
        activeProfile = 0;
        dirty = profiles.front().dirty;
        return true;
    }

    ayt::resource::ProjectBuildProfile* currentProfile()
    {
        if (activeProfile < 0
            || static_cast<std::size_t>(activeProfile) >= profiles.size()) {
            return nullptr;
        }
        return &profiles[static_cast<std::size_t>(activeProfile)].profile;
    }

    void refreshNavigation(int page)
    {
        if (nav == nullptr) return;
        refreshing = true;
        nav->setSelectedIndex(page);
        static constexpr const char* pages[] = {
            "settings_page_project", "settings_page_flow",
            "settings_page_worlds", "settings_page_build",
            "settings_page_rules", "settings_page_run",
            "settings_page_diagnostics"};
        for (int index = 0; index < static_cast<int>(std::size(pages)); ++index) {
            if (ayt::ui::Widget* widget = ui->findById(pages[index])) {
                widget->setVisible(index == page);
            }
        }
        if (auto* scroll = dynamic_cast<ayt::ui::ScrollView*>(
                ui->findById("settings_scroll"))) {
            scroll->setScrollOffset({0.0f, 0.0f});
        }
        ui->invalidateLayout();
        // Hidden pages have no child geometry until their first activation.
        // Layout synchronously so the first frame of a newly selected page is
        // fully arranged instead of briefly rendering every control at (0,0).
        ui->layout();
        refreshing = false;
    }

    void refreshProject()
    {
        refreshing = true;
        setText("settings_project_id", project.id);
        setText("settings_project_name", project.displayName);
        setText("settings_project_engine", project.engineProfile);
        setText("settings_project_assets", project.assetRoot);
        setText("settings_project_assembly", project.gameAssembly);
        setText("settings_project_code", project.gameCodeRoot);
        const int view = project.defaultSceneView == "2D" ? 1
            : project.defaultSceneView == "3D" ? 2 : 0;
        setSelected("settings_project_scene_view", view);
        setText("settings_gameflow", project.startupFlow);
        setText("settings_gameflow_contract", project.gameFlowContract);
        setText("settings_uiflow", project.ui.flow);
        setText("settings_uiflow_entry", project.ui.entry);
        refreshing = false;
        refreshWorldList();
    }

    void captureProject()
    {
        project.schemaVersion = kEditorProjectDescriptorSchemaVersion;
        project.id = text("settings_project_id");
        project.displayName = text("settings_project_name");
        project.engineProfile = text("settings_project_engine");
        project.assetRoot = text("settings_project_assets");
        project.gameAssembly = text("settings_project_assembly");
        project.gameCodeRoot = text("settings_project_code");
        const int view = selected("settings_project_scene_view");
        project.defaultSceneView = view == 1 ? "2D" : view == 2 ? "3D" : "Auto";
        project.startupFlow = text("settings_gameflow");
        project.gameFlowContract = text("settings_gameflow_contract");
        project.ui.flow = text("settings_uiflow");
        project.ui.entry = text("settings_uiflow_entry");
        captureWorld();
    }

    void refreshWorldList()
    {
        if (worldList == nullptr) return;
        refreshing = true;
        std::vector<std::wstring> items;
        items.reserve(project.worlds.size());
        for (const auto& world : project.worlds) {
            items.push_back(wide(world.id + "   —   " + world.scene));
        }
        worldList->setItems(items);
        if (project.worlds.empty()) selectedWorld = -1;
        else if (selectedWorld < 0
            || static_cast<std::size_t>(selectedWorld) >= project.worlds.size()) {
            selectedWorld = 0;
        }
        worldList->setSelectedIndex(selectedWorld);
        refreshWorldEditor();
        std::vector<std::wstring> startupItems{L"(None)"};
        for (const auto& world : project.worlds) startupItems.push_back(wide(world.id));
        if (startupWorld != nullptr) {
            startupWorld->setItems(startupItems);
            int index = 0;
            for (std::size_t i = 0; i < project.worlds.size(); ++i) {
                if (project.worlds[i].id == project.startupWorld) {
                    index = static_cast<int>(i + 1u);
                    break;
                }
            }
            startupWorld->setSelectedIndex(index);
        }
        refreshing = false;
    }

    void refreshWorldEditor()
    {
        const bool valid = selectedWorld >= 0
            && static_cast<std::size_t>(selectedWorld) < project.worlds.size();
        const EditorProjectWorldDescriptor empty;
        const auto& world = valid
            ? project.worlds[static_cast<std::size_t>(selectedWorld)] : empty;
        setText("settings_world_id", world.id);
        setText("settings_world_scene", world.scene);
        setText("settings_world_context", world.uiContext);
        setText("settings_world_tilemaps", joinLines(world.tilemaps));
    }

    void captureWorld()
    {
        if (selectedWorld < 0
            || static_cast<std::size_t>(selectedWorld) >= project.worlds.size()) {
            return;
        }
        auto& world = project.worlds[static_cast<std::size_t>(selectedWorld)];
        world.id = text("settings_world_id");
        world.scene = text("settings_world_scene");
        world.uiContext = text("settings_world_context");
        world.tilemaps = splitLines(text("settings_world_tilemaps"));
        if (startupWorld != nullptr) {
            const int index = startupWorld->getSelectedIndex();
            project.startupWorld = index > 0
                && static_cast<std::size_t>(index - 1) < project.worlds.size()
                ? project.worlds[static_cast<std::size_t>(index - 1)].id
                : std::string{};
        }
    }

    void refreshProfilePicker()
    {
        if (profilePicker == nullptr) return;
        refreshing = true;
        std::vector<std::wstring> items;
        items.reserve(profiles.size());
        for (const auto& entry : profiles) {
            items.push_back(wide(entry.profile.id));
        }
        profilePicker->setItems(items);
        profilePicker->setSelectedIndex(activeProfile);
        refreshing = false;
    }

    void refreshBuild()
    {
        auto* profile = currentProfile();
        if (profile == nullptr) return;
        refreshing = true;
        setText("settings_build_id", profile->id);
        setText("settings_build_platform", profile->platform);
        setText("settings_build_arch", profile->architecture);
        setText("settings_build_config", profile->configuration);
        setChecked("settings_code_enabled", profile->code.enabled);
        setText("settings_code_cmake", profile->code.cmakeExecutable);
        setText("settings_code_source", profile->code.sourceDirectory);
        setText("settings_code_configure", profile->code.configurePreset);
        setText("settings_code_build", profile->code.buildPreset);
        setText("settings_code_target", profile->code.target);
        setText("settings_code_artifact", profile->code.artifact);
        setText("settings_content_assets", profile->content.assetRoot);
        setText("settings_content_output", profile->content.outputSubdirectory);
        setSelected("settings_content_transform",
                    transformIndex(profile->content.defaultTransform));
        setSelected("settings_content_storage",
                    profile->content.defaultStorage
                        == ayt::resource::ProjectAssetStorage::Pak ? 1 : 0);
        setChecked("settings_cache_enabled", profile->cache.enabled);
        setText("settings_cache_root", profile->cache.root);
        setSelected("settings_cache_policy", policyIndex(profile->cache.policy));
        setText("settings_package_output", profile->package.output);
        setSelected("settings_package_compression",
            profile->package.compression == "none" ? 0
            : profile->package.compression == "lz4" ? 1 : 2);
        setChecked("settings_package_atomic", profile->package.atomic);
        setText("settings_package_workdir", profile->run.workingDirectory);
        setText("settings_package_arguments", joinLines(profile->run.arguments));
        setText("settings_run_executable", project.run.executable);
        setText("settings_run_workdir", project.run.workingDirectory);
        setText("settings_run_arguments", joinLines(project.run.arguments));
        refreshing = false;
        refreshRuleList();
    }

    void captureBuild()
    {
        auto* profile = currentProfile();
        if (profile == nullptr) return;
        profile->schemaVersion = 1u;
        profile->id = text("settings_build_id");
        profile->platform = text("settings_build_platform");
        profile->architecture = text("settings_build_arch");
        profile->configuration = text("settings_build_config");
        profile->code.enabled = checked("settings_code_enabled");
        profile->code.backend = "cmake";
        profile->code.cmakeExecutable = text("settings_code_cmake");
        profile->code.sourceDirectory = text("settings_code_source");
        profile->code.configurePreset = text("settings_code_configure");
        profile->code.buildPreset = text("settings_code_build");
        profile->code.target = text("settings_code_target");
        profile->code.artifact = text("settings_code_artifact");
        profile->content.assetRoot = text("settings_content_assets");
        profile->content.outputSubdirectory = text("settings_content_output");
        profile->content.defaultTransform = transformAt(
            selected("settings_content_transform"));
        profile->content.defaultStorage = selected("settings_content_storage") == 1
            ? ayt::resource::ProjectAssetStorage::Pak
            : ayt::resource::ProjectAssetStorage::Loose;
        profile->cache.enabled = checked("settings_cache_enabled");
        profile->cache.root = text("settings_cache_root");
        profile->cache.policy = policyAt(selected("settings_cache_policy"));
        profile->package.output = text("settings_package_output");
        const int compression = selected("settings_package_compression");
        profile->package.compression = compression == 0 ? "none"
            : compression == 1 ? "lz4" : "zstd";
        profile->package.atomic = checked("settings_package_atomic");
        profile->run.workingDirectory = text("settings_package_workdir");
        profile->run.arguments = splitLines(text("settings_package_arguments"));
        project.run.executable = text("settings_run_executable");
        project.run.workingDirectory = text("settings_run_workdir");
        project.run.arguments = splitLines(text("settings_run_arguments"));
        captureRule();
        profiles[static_cast<std::size_t>(activeProfile)].dirty = true;
    }

    void refreshRuleList()
    {
        if (ruleList == nullptr) return;
        auto* profile = currentProfile();
        if (profile == nullptr) return;
        refreshing = true;
        std::vector<std::wstring> items;
        for (std::size_t index = 0; index < profile->content.rules.size(); ++index) {
            items.push_back(ruleLabel(profile->content.rules[index], index));
        }
        ruleList->setItems(items);
        if (profile->content.rules.empty()) selectedRule = -1;
        else if (selectedRule < 0
            || static_cast<std::size_t>(selectedRule)
                >= profile->content.rules.size()) selectedRule = 0;
        ruleList->setSelectedIndex(selectedRule);
        refreshRuleEditor();
        refreshing = false;
    }

    void refreshRuleEditor()
    {
        auto* profile = currentProfile();
        const bool valid = profile != nullptr && selectedRule >= 0
            && static_cast<std::size_t>(selectedRule)
                < profile->content.rules.size();
        ayt::resource::ProjectBuildRule empty;
        const auto& rule = valid
            ? profile->content.rules[static_cast<std::size_t>(selectedRule)]
            : empty;
        setText("settings_rule_match", rule.match);
        setSelected("settings_rule_transform", transformIndex(rule.transform));
        setSelected("settings_rule_storage",
                    rule.storage == ayt::resource::ProjectAssetStorage::Pak ? 1 : 0);
        setText("settings_rule_chunk", rule.chunk);
        setChecked("settings_rule_cook_textures", rule.cookTextures);
    }

    void captureRule()
    {
        auto* profile = currentProfile();
        if (profile == nullptr || selectedRule < 0
            || static_cast<std::size_t>(selectedRule)
                >= profile->content.rules.size()) return;
        auto& rule = profile->content.rules[static_cast<std::size_t>(selectedRule)];
        rule.match = text("settings_rule_match");
        rule.transform = transformAt(selected("settings_rule_transform"));
        rule.storage = selected("settings_rule_storage") == 1
            ? ayt::resource::ProjectAssetStorage::Pak
            : ayt::resource::ProjectAssetStorage::Loose;
        rule.chunk = text("settings_rule_chunk");
        rule.cookTextures = checked("settings_rule_cook_textures");
    }

    bool validateAll(std::string* error)
    {
        captureProject();
        captureBuild();
        if (!project.validate(error)) return false;
        auto* profile = currentProfile();
        return profile != nullptr && profile->validate(error);
    }

    bool saveAll(std::string* error)
    {
        if (!validateAll(error)) return false;
        if (!project.save(config.projectRoot, error)) return false;
        for (BuildProfileEntry& entry : profiles) {
            if (!entry.profile.save(entry.path, error)) return false;
            entry.profile.sourcePath = fs::absolute(entry.path)
                .lexically_normal().string();
            entry.dirty = false;
        }
        dirty = false;
        return true;
    }

    void populateDiagnostics(const ayt::resource::ProjectBuildPlan& plan)
    {
        if (diagnostics == nullptr) return;
        std::vector<std::wstring> items;
        items.push_back(L"Assets: " + std::to_wstring(plan.assets.size()));
        std::size_t raw = 0, cook = 0, excluded = 0, pak = 0;
        for (const auto& asset : plan.assets) {
            if (asset.transform == ayt::resource::ProjectAssetTransform::Raw) ++raw;
            else if (asset.transform == ayt::resource::ProjectAssetTransform::Cook) ++cook;
            else if (asset.transform == ayt::resource::ProjectAssetTransform::Exclude) ++excluded;
            if (asset.storage == ayt::resource::ProjectAssetStorage::Pak) ++pak;
        }
        items.push_back(L"Raw: " + std::to_wstring(raw)
            + L"   Cook: " + std::to_wstring(cook)
            + L"   Excluded: " + std::to_wstring(excluded)
            + L"   Pak: " + std::to_wstring(pak));
        for (const auto& item : plan.diagnostics) {
            const wchar_t* prefix = item.severity
                == ayt::resource::ProjectBuildDiagnostic::Severity::Error
                ? L"ERROR" : item.severity
                    == ayt::resource::ProjectBuildDiagnostic::Severity::Warning
                ? L"WARNING" : L"INFO";
            items.push_back(std::wstring(prefix) + L"  " + wide(item.asset)
                + (item.asset.empty() ? L"" : L" — ") + wide(item.message));
        }
        diagnostics->setItems(items);
    }

    void createPlan()
    {
        std::string error;
        if (!validateAll(&error)) {
            statusText(error, true);
            refreshNavigation(6);
            return;
        }
        plan = ayt::resource::ProjectBuildPlanner::create(
            *currentProfile(), config.projectRoot);
        populateDiagnostics(plan);
        refreshNavigation(6);
        statusText(plan.valid() ? "Build plan is valid."
                                : "Build plan contains errors.", !plan.valid());
    }

    void startBuild(ayt::resource::ProjectBuildExecutionOptions options,
                    bool runAfter)
    {
        if (isBusy()) return;
        std::string error;
        if (!saveAll(&error)) {
            statusText(error, true);
            return;
        }
        plan = ayt::resource::ProjectBuildPlanner::create(
            *currentProfile(), config.projectRoot);
        populateDiagnostics(plan);
        if (!plan.valid()) {
            refreshNavigation(6);
            statusText("Build plan contains errors.", true);
            return;
        }
        asyncState = std::make_shared<AsyncState>();
        runWhenFinished = runAfter;
        if (progress != nullptr) {
            progress->setIndeterminate(false);
            progress->setValue(0.0f);
        }
        const auto state = asyncState;
        const auto buildPlan = plan;
        buildFuture = std::async(std::launch::async,
            [state, buildPlan, options]() {
                auto result = ayt::resource::ProjectBuildExecutor::execute(
                    buildPlan, options,
                    [state](const ayt::resource::ProjectBuildProgress& value) {
                        std::lock_guard lock(state->mutex);
                        state->progress = value;
                    });
                std::lock_guard lock(state->mutex);
                state->result = std::move(result);
                state->finished = true;
            });
        statusText(options.dryRun ? "Running build validation..."
                                  : "Building project...");
    }

    bool isBusy() const noexcept
    {
        return buildFuture.valid()
            && buildFuture.wait_for(std::chrono::seconds(0))
                != std::future_status::ready;
    }

    void pollBuild()
    {
        if (asyncState == nullptr) return;
        ayt::resource::ProjectBuildProgress current;
        ayt::resource::ProjectBuildResult result;
        bool finished = false;
        {
            std::lock_guard lock(asyncState->mutex);
            current = asyncState->progress;
            finished = asyncState->finished;
            if (finished) result = asyncState->result;
        }
        if (progress != nullptr) progress->setValue(current.fraction);
        if (!finished) {
            if (!current.message.empty()) statusText(current.message);
            return;
        }
        if (buildFuture.valid()) buildFuture.get();
        if (progress != nullptr) progress->setValue(result.ok ? 1.0f : 0.0f);
        std::vector<std::wstring> items;
        items.push_back(result.ok ? L"BUILD SUCCEEDED" : L"BUILD FAILED");
        items.push_back(L"Raw " + std::to_wstring(result.rawCount)
            + L"   Cooked " + std::to_wstring(result.cookedCount)
            + L"   Cache hits " + std::to_wstring(result.cacheHitCount)
            + L"   Pak files " + std::to_wstring(result.pakFileCount));
        if (!result.outputDirectory.empty()) {
            items.push_back(L"Output: " + wide(result.outputDirectory));
        }
        for (const auto& item : result.diagnostics) {
            items.push_back(wide(item.asset + (item.asset.empty() ? "" : " — ")
                + item.message));
        }
        if (!result.error.empty()) items.push_back(L"ERROR  " + wide(result.error));
        if (diagnostics != nullptr) diagnostics->setItems(items);
        refreshNavigation(6);
        statusText(result.ok ? "Project build completed."
                             : result.error, !result.ok);
        const bool shouldRun = result.ok && runWhenFinished;
        asyncState.reset();
        runWhenFinished = false;
        if (shouldRun && config.runProject) (void)config.runProject();
    }

    void chooseInto(const char* id, EditorProjectPathKind kind,
                    bool relativeToAssets)
    {
        if (!config.choosePath) return;
        const std::string chosen = config.choosePath(kind);
        if (chosen.empty()) return;
        const fs::path base = relativeToAssets
            ? fs::path(config.projectRoot)
                / (project.assetRoot.empty() ? "Assets" : project.assetRoot)
            : fs::path(config.projectRoot);
        setText(id, relativePortable(base, chosen));
        markDirty();
    }

    bool attach(ayt::ui::UIManager& manager, std::string* error)
    {
        ui = &manager;
        nav = find<ayt::ui::ListView>("settings_navigation");
        worldList = find<ayt::ui::ListView>("settings_world_list");
        ruleList = find<ayt::ui::ListView>("settings_rule_list");
        diagnostics = find<ayt::ui::ListView>("settings_diagnostics");
        profilePicker = find<ayt::ui::ComboBox>("settings_profile_picker");
        startupWorld = find<ayt::ui::ComboBox>("settings_startup_world");
        status = find<ayt::ui::TextLabel>("settings_status");
        progress = find<ayt::ui::ProgressBar>("settings_progress");
        if (nav == nullptr || worldList == nullptr || ruleList == nullptr
            || diagnostics == nullptr || profilePicker == nullptr
            || startupWorld == nullptr || status == nullptr
            || progress == nullptr) {
            if (error != nullptr) {
                *error = "Project Settings layout is missing required controls.";
            }
            detach();
            return false;
        }
        if (!load(error)) {
            detach();
            return false;
        }

        nav->setItems({
            localized("ui.editor.project_settings.nav_project", L"Project"),
            localized("ui.editor.project_settings.nav_flow", L"Flow"),
            localized("ui.editor.project_settings.nav_worlds", L"Worlds"),
            localized("ui.editor.project_settings.nav_build", L"Build"),
            localized("ui.editor.project_settings.nav_rules", L"Content Rules"),
            localized("ui.editor.project_settings.nav_run", L"Run"),
            localized("ui.editor.project_settings.nav_diagnostics", L"Diagnostics"),
        });
        nav->setOnSelectionChanged([this](int index) {
            if (!refreshing && index >= 0) refreshNavigation(index);
        });
        worldList->setOnSelectionChanged([this](int index) {
            if (refreshing) return;
            captureWorld();
            selectedWorld = index;
            refreshing = true;
            refreshWorldEditor();
            refreshing = false;
        });
        ruleList->setOnSelectionChanged([this](int index) {
            if (refreshing) return;
            captureRule();
            selectedRule = index;
            refreshing = true;
            refreshRuleEditor();
            refreshing = false;
        });
        profilePicker->setOnSelectionChanged([this](int index) {
            if (refreshing || index < 0
                || static_cast<std::size_t>(index) >= profiles.size()) return;
            captureBuild();
            activeProfile = index;
            selectedRule = -1;
            refreshBuild();
        });
        startupWorld->setOnSelectionChanged([this](int index) {
            if (refreshing) return;
            project.startupWorld = index > 0
                && static_cast<std::size_t>(index - 1) < project.worlds.size()
                ? project.worlds[static_cast<std::size_t>(index - 1)].id
                : std::string{};
            markDirty();
        });

        static constexpr const char* inputs[] = {
            "settings_project_id", "settings_project_name",
            "settings_project_engine", "settings_project_assets",
            "settings_project_assembly", "settings_project_code",
            "settings_gameflow", "settings_gameflow_contract",
            "settings_uiflow", "settings_uiflow_entry",
            "settings_world_id", "settings_world_scene",
            "settings_world_context", "settings_world_tilemaps",
            "settings_build_id", "settings_build_platform",
            "settings_build_arch", "settings_build_config",
            "settings_code_cmake", "settings_code_source",
            "settings_code_configure", "settings_code_build",
            "settings_code_target", "settings_code_artifact",
            "settings_content_assets", "settings_content_output",
            "settings_cache_root", "settings_package_output",
            "settings_package_workdir", "settings_package_arguments",
            "settings_rule_match", "settings_rule_chunk",
            "settings_run_executable", "settings_run_workdir",
            "settings_run_arguments"};
        for (const char* id : inputs) bindDirtyInput(id);
        static constexpr const char* checks[] = {
            "settings_code_enabled", "settings_cache_enabled",
            "settings_package_atomic", "settings_rule_cook_textures"};
        for (const char* id : checks) bindDirtyCheck(id);
        static constexpr const char* combos[] = {
            "settings_project_scene_view", "settings_content_transform",
            "settings_content_storage", "settings_cache_policy",
            "settings_package_compression", "settings_rule_transform",
            "settings_rule_storage"};
        for (const char* id : combos) bindDirtyCombo(id);

        bindButton("settings_btn_save", [this]() {
            std::string saveError;
            if (saveAll(&saveError)) statusText("Project settings saved.");
            else statusText(saveError, true);
        });
        bindButton("settings_btn_reload", [this]() {
            std::string loadError;
            if (!load(&loadError)) {
                statusText(loadError, true);
                return;
            }
            refreshProfilePicker();
            refreshProject();
            refreshBuild();
            refreshNavigation(0);
            statusText("Project settings reloaded.");
        });
        bindButton("settings_btn_plan", [this]() { createPlan(); });
        bindButton("settings_btn_dry_run", [this]() {
            ayt::resource::ProjectBuildExecutionOptions options;
            options.dryRun = true;
            startBuild(options, false);
        });
        bindButton("settings_btn_build", [this]() {
            startBuild({}, false);
        });
        bindButton("settings_btn_build_run", [this]() {
            startBuild({}, true);
        });
        bindButton("settings_btn_run", [this]() {
            if (config.runProject) (void)config.runProject();
        });
        bindButton("settings_btn_new_profile", [this]() {
            captureBuild();
            auto profile = defaultProfile(project.id);
            std::size_t suffix = profiles.size() + 1u;
            profile.id = "new-profile-" + std::to_string(suffix);
            fs::path path;
            do {
                path = fs::path(config.projectRoot) / "BuildProfiles"
                    / (profile.id + ".aybuild.json");
                ++suffix;
            } while (fs::exists(path));
            profile.sourcePath = path.string();
            profiles.push_back({path.string(), std::move(profile), true});
            activeProfile = static_cast<int>(profiles.size() - 1u);
            selectedRule = -1;
            refreshProfilePicker();
            refreshBuild();
            markDirty();
        });
        bindButton("settings_btn_duplicate_profile", [this]() {
            captureBuild();
            auto* current = currentProfile();
            if (current == nullptr) return;
            auto copy = *current;
            copy.id += "-copy";
            fs::path path = fs::path(config.projectRoot) / "BuildProfiles"
                / (copy.id + ".aybuild.json");
            for (int suffix = 2; fs::exists(path); ++suffix) {
                path = fs::path(config.projectRoot) / "BuildProfiles"
                    / (copy.id + "-" + std::to_string(suffix)
                        + ".aybuild.json");
            }
            copy.sourcePath = path.string();
            profiles.push_back({path.string(), std::move(copy), true});
            activeProfile = static_cast<int>(profiles.size() - 1u);
            selectedRule = -1;
            refreshProfilePicker();
            refreshBuild();
            markDirty();
        });
        bindButton("settings_btn_add_world", [this]() {
            captureWorld();
            const std::size_t number = project.worlds.size() + 1u;
            project.worlds.push_back({"world_" + std::to_string(number),
                "worlds/world_" + std::to_string(number) + ".ayscene",
                {}, {}, {}});
            selectedWorld = static_cast<int>(project.worlds.size() - 1u);
            refreshWorldList();
            markDirty();
        });
        bindButton("settings_btn_remove_world", [this]() {
            if (selectedWorld < 0
                || static_cast<std::size_t>(selectedWorld) >= project.worlds.size()) return;
            const std::string removed = project.worlds[static_cast<std::size_t>(selectedWorld)].id;
            project.worlds.erase(project.worlds.begin() + selectedWorld);
            if (project.startupWorld == removed) project.startupWorld.clear();
            if (selectedWorld >= static_cast<int>(project.worlds.size())) {
                selectedWorld = static_cast<int>(project.worlds.size()) - 1;
            }
            refreshWorldList();
            markDirty();
        });
        bindButton("settings_btn_apply_world", [this]() {
            captureWorld();
            refreshWorldList();
            markDirty();
        });
        bindButton("settings_btn_add_rule", [this]() {
            captureRule();
            auto* profile = currentProfile();
            if (profile == nullptr) return;
            profile->content.rules.push_back({"**/*",
                ayt::resource::ProjectAssetTransform::Auto,
                ayt::resource::ProjectAssetStorage::Loose, "core", true});
            selectedRule = static_cast<int>(profile->content.rules.size() - 1u);
            refreshRuleList();
            markDirty();
        });
        bindButton("settings_btn_remove_rule", [this]() {
            auto* profile = currentProfile();
            if (profile == nullptr || selectedRule < 0
                || static_cast<std::size_t>(selectedRule)
                    >= profile->content.rules.size()) return;
            profile->content.rules.erase(
                profile->content.rules.begin() + selectedRule);
            if (selectedRule >= static_cast<int>(profile->content.rules.size())) {
                selectedRule = static_cast<int>(profile->content.rules.size()) - 1;
            }
            refreshRuleList();
            markDirty();
        });
        bindButton("settings_btn_rule_up", [this]() {
            auto* profile = currentProfile();
            if (profile == nullptr || selectedRule <= 0) return;
            captureRule();
            std::swap(profile->content.rules[static_cast<std::size_t>(selectedRule)],
                      profile->content.rules[static_cast<std::size_t>(selectedRule - 1)]);
            --selectedRule;
            refreshRuleList();
            markDirty();
        });
        bindButton("settings_btn_rule_down", [this]() {
            auto* profile = currentProfile();
            if (profile == nullptr || selectedRule < 0
                || static_cast<std::size_t>(selectedRule + 1)
                    >= profile->content.rules.size()) return;
            captureRule();
            std::swap(profile->content.rules[static_cast<std::size_t>(selectedRule)],
                      profile->content.rules[static_cast<std::size_t>(selectedRule + 1)]);
            ++selectedRule;
            refreshRuleList();
            markDirty();
        });
        bindButton("settings_btn_apply_rule", [this]() {
            captureRule();
            refreshRuleList();
            markDirty();
        });

        bindButton("settings_btn_pick_gameflow", [this]() {
            chooseInto("settings_gameflow", EditorProjectPathKind::GameFlow, true);
        });
        bindButton("settings_btn_pick_contract", [this]() {
            chooseInto("settings_gameflow_contract",
                       EditorProjectPathKind::GameFlowContract, true);
        });
        bindButton("settings_btn_pick_uiflow", [this]() {
            chooseInto("settings_uiflow", EditorProjectPathKind::UiFlow, true);
        });
        bindButton("settings_btn_pick_scene", [this]() {
            chooseInto("settings_world_scene", EditorProjectPathKind::Scene, true);
        });
        bindButton("settings_btn_pick_assembly", [this]() {
            chooseInto("settings_project_assembly",
                       EditorProjectPathKind::GameAssembly, false);
        });
        bindButton("settings_btn_pick_executable", [this]() {
            chooseInto("settings_run_executable",
                       EditorProjectPathKind::Executable, false);
        });
        bindButton("settings_btn_pick_artifact", [this]() {
            chooseInto("settings_code_artifact",
                       EditorProjectPathKind::BuildArtifact, false);
        });
        bindButton("settings_btn_open_gameflow", [this]() {
            captureProject();
            if (config.openGameFlow) (void)config.openGameFlow(project.startupFlow);
        });
        bindButton("settings_btn_open_uiflow", [this]() {
            captureProject();
            if (config.openUiFlow) (void)config.openUiFlow(project.ui.flow);
        });

        refreshProfilePicker();
        refreshProject();
        refreshBuild();
        refreshNavigation(0);
        progress->setValueRange(0.0f, 1.0f);
        progress->setValue(0.0f);
        attached = true;
        statusText(dirty
            ? "A default build profile is ready. Save All to create it."
            : "Project settings loaded.");
        return true;
    }

    void detach()
    {
        attached = false;
        ui = nullptr;
        nav = nullptr;
        worldList = nullptr;
        ruleList = nullptr;
        diagnostics = nullptr;
        profilePicker = nullptr;
        startupWorld = nullptr;
        status = nullptr;
        progress = nullptr;
    }

    EditorProjectSettingsConfig config;
    EditorProjectDescriptor project;
    std::vector<BuildProfileEntry> profiles;
    int activeProfile = -1;
    int selectedWorld = -1;
    int selectedRule = -1;
    ayt::resource::ProjectBuildPlan plan;
    std::shared_ptr<AsyncState> asyncState;
    std::future<void> buildFuture;
    bool runWhenFinished = false;

    ayt::ui::UIManager* ui = nullptr;
    ayt::ui::ListView* nav = nullptr;
    ayt::ui::ListView* worldList = nullptr;
    ayt::ui::ListView* ruleList = nullptr;
    ayt::ui::ListView* diagnostics = nullptr;
    ayt::ui::ComboBox* profilePicker = nullptr;
    ayt::ui::ComboBox* startupWorld = nullptr;
    ayt::ui::TextLabel* status = nullptr;
    ayt::ui::ProgressBar* progress = nullptr;
    bool attached = false;
    bool refreshing = false;
    bool dirty = false;
    std::string lastError;
};

EditorProjectSettingsController::EditorProjectSettingsController(
    EditorProjectSettingsConfig config)
    : _impl(std::make_unique<Impl>(std::move(config)))
{
}

EditorProjectSettingsController::~EditorProjectSettingsController() = default;

bool EditorProjectSettingsController::attach(
    ayt::ui::UIManager& ui, std::string* error)
{
    return _impl != nullptr && _impl->attach(ui, error);
}

void EditorProjectSettingsController::detach()
{
    if (_impl != nullptr) _impl->detach();
}

void EditorProjectSettingsController::tick(float)
{
    if (_impl != nullptr) _impl->pollBuild();
}

bool EditorProjectSettingsController::save(std::string* error)
{
    return _impl != nullptr && _impl->saveAll(error);
}

bool EditorProjectSettingsController::isAttached() const noexcept
{
    return _impl != nullptr && _impl->attached;
}

bool EditorProjectSettingsController::isBusy() const noexcept
{
    return _impl != nullptr && _impl->isBusy();
}

bool EditorProjectSettingsController::isDirty() const noexcept
{
    return _impl != nullptr && _impl->dirty;
}

const std::string& EditorProjectSettingsController::lastError() const noexcept
{
    static const std::string empty;
    return _impl != nullptr ? _impl->lastError : empty;
}

} // namespace ayt::editor
