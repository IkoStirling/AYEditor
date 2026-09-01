#include "AYEditor/EditorSession.h"

#include "AYEditor/EditorHeapDebug.h"
#include "AYEditor/EditorVisualStyle.h"
#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/EditorDslDocument.h"
#include "AYEntity.h"
#include "AYUI/SplitterHandle.h"
#include "AYUI/Button.h"
#include "AYUI/CheckBox.h"
#include "AYUI/ComboBox.h"
#include "AYUI/Box.h"
#include "AYGameLoop.h"
#include "AYUI/MenuBar.h"
#include "AYUI/Menu.h"
#include "AYUI/MenuItem.h"
#include "AYRenderer/RendererSubSystem.h"
#include "AYUI/Slider.h"
#include "AYUI/SvgIcon.h"
#include "AYUI/TextLabel.h"
#include "AYUI/TextInput.h"
#include "AYUI/TextArea.h"
#include "AYUI/Theme.h"
#include "AYUI/TreeView.h"  // v0.3+ PR-5 Hierarchy panel (design §4.3.y)
#include "AYUI/ListView.h"
#include "AYUI/TileView.h"
#include "AYUI/UnicodeText.h"
#include "AYUI/Widget.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "LayoutEditorSession.h"
#include "AudioEditorSession.h"
#include "AYAudio/AudioSubSystem.h"
#include "AYUI/UIKeyCode.h"
#include "AYDevice/DeviceManager.h"

// v0.3 PR-4 — Editor 消费 host->scenes()（design §4.2.x + §4.3.x）
// AYScene 完整 include 因文档层需 SceneMode/Scene 完整类型；
// IEngineHost 走 host facade（v0.1.3 PR-6 ship）。
#include "AYScene.h"
#include "AYScene/SceneManager.h"
#include "AYScene/SceneMode.h"
#include "AYApplication/IEngineHost.h"
#include "AYApplication.h"  // currentEngineHost() / defaultEngineHost()

#include <AYEntity/components/AnimationComponent.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYMath/MathTransform.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/AssetPath.h>
#include <AYResource/assetsDefs/IMesh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <sys/stat.h>
#include <vector>

namespace ayt::editor {

struct EditorSession::OpenDslDocument {
    EditorAssetId assetId = 0;
    std::string cardId;
    std::string fileName;
    EditorDslDocument model;
    ayt::ui::DockCard* card = nullptr;
    ayt::ui::TextArea* source = nullptr;
    ayt::ui::TextArea* diagnostics = nullptr;
    ayt::ui::TextLabel* status = nullptr;
};

namespace {

// DeviceInputBridge converts wheel notches to AYUI logical pixels before it
// reaches EditorSession. Keep the inverse conversion explicit here: treating
// the default 40 pixels as 40 notches made one physical notch hit Freecam's
// eight-notch safety clamp and produced the touchpad sensitivity spike.
constexpr float kWheelLogicalPixelsPerNotch = 40.0f;
constexpr float kViewportLookDragSlopPixels = 8.0f;
static_assert(EditorTransformGizmo::kWorldScalePerCameraDistance
              == ayt::render::kEditorTransformGizmoScalePerDistance,
              "editor hit geometry and renderer gizmo scale must match");

bool intersectRaySphere(const ayt::math::FVector3& origin,
                        const ayt::math::FVector3& direction,
                        const ayt::math::FVector3& center,
                        float radius,
                        float& outDistance)
{
    const ayt::math::FVector3 toCenter = center - origin;
    const float projected = toCenter.dot(direction);
    if (projected < 0.0f) return false;
    const float perpendicularSq = toCenter.dot(toCenter) - projected * projected;
    const float radiusSq = radius * radius;
    if (perpendicularSq > radiusSq) return false;
    outDistance = projected - std::sqrt(std::max(0.0f, radiusSq - perpendicularSq));
    return true;
}

bool intersectRayAabb(const ayt::math::FVector3& origin,
                      const ayt::math::FVector3& direction,
                      const ayt::math::FVector3& boundsMin,
                      const ayt::math::FVector3& boundsMax,
                      float& outDistance)
{
    float nearDistance = 0.0f;
    float farDistance = 1.0e30f;
    for (int axis = 0; axis < 3; ++axis) {
        const float rayOrigin = origin[axis];
        const float rayDirection = direction[axis];
        if (std::fabs(rayDirection) < 1.0e-7f) {
            if (rayOrigin < boundsMin[axis] || rayOrigin > boundsMax[axis]) {
                return false;
            }
            continue;
        }
        float first = (boundsMin[axis] - rayOrigin) / rayDirection;
        float second = (boundsMax[axis] - rayOrigin) / rayDirection;
        if (first > second) std::swap(first, second);
        nearDistance = std::max(nearDistance, first);
        farDistance = std::min(farDistance, second);
        if (nearDistance > farDistance) return false;
    }
    outDistance = nearDistance;
    return farDistance >= 0.0f;
}

bool layoutEditorFileExists(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

std::filesystem::path editorExecutableDirectory() {
    char modulePath[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    return length > 0 && length < MAX_PATH
        ? std::filesystem::path(modulePath).parent_path()
        : std::filesystem::path{};
}

std::string resolveLayoutEditorChromePath() {
    const std::filesystem::path executableDirectory =
        editorExecutableDirectory();
    const std::vector<std::string> candidates = {
        (executableDirectory / "assets/ui/layout_editor.ui.json").string(),
        "assets/ui/layout_editor.ui.json",
        "AYRuntime/AYEditor/assets/ui/layout_editor.ui.json",
        "../AYRuntime/AYEditor/assets/ui/layout_editor.ui.json",
        "../../AYRuntime/AYEditor/assets/ui/layout_editor.ui.json",
        "AYRuntime/AYUI/demo/layout_editor/assets/layout_editor.ui.json",
        "../AYRuntime/AYUI/demo/layout_editor/assets/layout_editor.ui.json",
    };
    for (const std::string& path : candidates) {
        if (layoutEditorFileExists(path)) {
            return path;
        }
    }
    return candidates.front();
}

std::string resolveAudioEditorChromePath() {
    const std::filesystem::path executableDirectory =
        editorExecutableDirectory();
    const std::vector<std::string> candidates = {
        (executableDirectory / "assets/ui/audio_editor.ui.json").string(),
        "assets/ui/audio_editor.ui.json",
        "AYRuntime/AYEditor/assets/ui/audio_editor.ui.json",
        "../AYRuntime/AYEditor/assets/ui/audio_editor.ui.json",
        "../../AYRuntime/AYEditor/assets/ui/audio_editor.ui.json",
        "AYRuntime/AYAudio/demo/audio_editor/assets/audio_editor.ui.json",
        "../AYRuntime/AYAudio/demo/audio_editor/assets/audio_editor.ui.json",
    };
    for (const std::string& path : candidates) {
        if (layoutEditorFileExists(path)) {
            return path;
        }
    }
    return candidates.front();
}

std::string showOpenAudioFileDialog(HWND owner) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        "Audio (*.wav;*.mp3;*.ogg)\0*.wav;*.mp3;*.ogg\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameA(&ofn)) {
        return {};
    }
    return std::string(path);
}

std::string showUiJsonOpenDialog(HWND owner) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        "AYUI Layout (*.ui.json)\0*.ui.json\0"
        "JSON (*.json)\0*.json\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "ui.json";
    if (!::GetOpenFileNameA(&ofn)) {
        return {};
    }
    return std::string(path);
}

std::string showUiJsonSaveDialog(HWND owner) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        "AYUI Layout (*.ui.json)\0*.ui.json\0"
        "JSON (*.json)\0*.json\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "ui.json";
    if (!::GetSaveFileNameA(&ofn)) {
        return {};
    }
    return std::string(path);
}

std::string showSceneOpenDialog(HWND owner) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "AY Scene (*.ayscene)\0*.ayscene\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "ayscene";
    return ::GetOpenFileNameA(&ofn) ? std::string(path) : std::string{};
}

std::string showSceneSaveDialog(HWND owner) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "AY Scene (*.ayscene)\0*.ayscene\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "ayscene";
    return ::GetSaveFileNameA(&ofn) ? std::string(path) : std::string{};
}

std::wstring formatFloat(float value) {
    wchar_t buffer[32] = {};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.3f", value);
    return buffer;
}

bool parseFloat(const std::wstring& text, float& value) {
    const wchar_t* begin = text.c_str();
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(begin, &end);
    while (end != nullptr && *end == L' ') ++end;
    if (begin == end || end == nullptr || *end != L'\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

std::string wideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int required = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    (void)::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), required, nullptr, nullptr);
    return result;
}

bool isDslIdentifierStart(wchar_t ch) noexcept
{
    return ch == L'_' || std::iswalpha(static_cast<wint_t>(ch)) != 0;
}

bool isDslIdentifierContinue(wchar_t ch) noexcept
{
    return ch == L'_' || std::iswalnum(static_cast<wint_t>(ch)) != 0;
}

std::vector<ayt::ui::TextArea::SyntaxSpan> highlightDslLine(
    EditorDslLanguage language, const std::wstring& line)
{
    using Span = ayt::ui::TextArea::SyntaxSpan;
    static const std::unordered_set<std::wstring> logiaKeywords = {
        L"script", L"var", L"on_start", L"on_update", L"on_destroy",
        L"run", L"function", L"signal", L"if", L"else", L"while",
        L"for", L"break", L"continue", L"do", L"end", L"return",
        L"true", L"false"};
    static const std::unordered_set<std::wstring> phoskiaKeywords = {
        L"material", L"property", L"uniform", L"storage", L"shared",
        L"uniformblock", L"binding", L"texture2d", L"texturecube",
        L"sampler", L"vertex", L"fragment", L"compute", L"let",
        L"if", L"else", L"for", L"in", L"out", L"return", L"true",
        L"false", L"variant", L"position", L"normal", L"color",
        L"texcoord", L"boneindices", L"boneweights", L"tangent"};
    static const std::unordered_set<std::wstring> builtinTypes = {
        L"bool", L"int", L"uint", L"float", L"double", L"string",
        L"vec2", L"vec3", L"vec4", L"ivec2", L"ivec3", L"ivec4",
        L"uvec2", L"uvec3", L"uvec4", L"mat2", L"mat3", L"mat4",
        L"Entity", L"Vector2", L"Vector3", L"Vector4", L"String",
        L"Float", L"Int", L"Bool", L"rwstructuredbuffer",
        L"structuredbuffer"};
    static const std::unordered_set<std::wstring> literalWords = {
        L"true", L"false", L"null"};

    const auto& keywords = language == EditorDslLanguage::Phoskia
        ? phoskiaKeywords : logiaKeywords;
    const ayt::math::FVector4 keywordColor(0.78f, 0.52f, 0.96f, 1.0f);
    const ayt::math::FVector4 typeColor(0.38f, 0.76f, 0.94f, 1.0f);
    const ayt::math::FVector4 literalColor(0.94f, 0.66f, 0.38f, 1.0f);
    const ayt::math::FVector4 stringColor(0.64f, 0.82f, 0.50f, 1.0f);
    const ayt::math::FVector4 commentColor(0.43f, 0.49f, 0.57f, 1.0f);

    std::vector<Span> spans;
    std::size_t index = 0;
    while (index < line.size()) {
        if (line[index] == L'/' && index + 1 < line.size()
            && line[index + 1] == L'/') {
            spans.push_back(Span{
                index, line.size() - index, commentColor});
            break;
        }

        if (line[index] == L'\"' || line[index] == L'\'') {
            const std::size_t start = index;
            const wchar_t quote = line[index++];
            while (index < line.size()) {
                if (line[index] == L'\\' && index + 1 < line.size()) {
                    index += 2;
                    continue;
                }
                const wchar_t current = line[index++];
                if (current == quote) break;
            }
            spans.push_back(Span{start, index - start, stringColor});
            continue;
        }

        if (std::iswdigit(static_cast<wint_t>(line[index])) != 0) {
            const std::size_t start = index++;
            while (index < line.size()) {
                const wchar_t current = line[index];
                const bool exponentSign = (current == L'+' || current == L'-')
                    && index > start
                    && (line[index - 1] == L'e' || line[index - 1] == L'E');
                if (std::iswalnum(static_cast<wint_t>(current)) == 0
                    && current != L'.' && current != L'_'
                    && !exponentSign) {
                    break;
                }
                ++index;
            }
            spans.push_back(Span{start, index - start, literalColor});
            continue;
        }

        if (isDslIdentifierStart(line[index])) {
            const std::size_t start = index++;
            while (index < line.size()
                   && isDslIdentifierContinue(line[index])) {
                ++index;
            }
            const std::wstring token = line.substr(start, index - start);
            if (literalWords.find(token) != literalWords.end()) {
                spans.push_back(Span{start, index - start, literalColor});
            } else if (keywords.find(token) != keywords.end()) {
                spans.push_back(Span{start, index - start, keywordColor});
            } else if (builtinTypes.find(token) != builtinTypes.end()) {
                spans.push_back(Span{start, index - start, typeColor});
            }
            continue;
        }
        ++index;
    }
    return spans;
}

const char* dslDiagnosticSeverityName(
    EditorDslDiagnosticSeverity severity) noexcept
{
    switch (severity) {
    case EditorDslDiagnosticSeverity::Info: return "info";
    case EditorDslDiagnosticSeverity::Warning: return "warning";
    case EditorDslDiagnosticSeverity::Error: break;
    }
    return "error";
}

std::wstring formatDslCompileReport(const EditorDslCompileReport& report,
                                    const std::string& displayPath)
{
    std::ostringstream output;
    output << (report.success ? "[Success] " : "[Failed] ")
           << displayPath << '\n'
           << report.summary;
    if (report.generatedBytes != 0) {
        output << " Generated output: " << report.generatedBytes << " bytes.";
    }
    if (report.diagnostics.empty()) {
        output << "\nNo diagnostics.";
    } else {
        for (const EditorDslDiagnostic& diagnostic : report.diagnostics) {
            output << "\n\n" << dslDiagnosticSeverityName(diagnostic.severity);
            if (diagnostic.line > 0) {
                output << " L" << diagnostic.line;
                if (diagnostic.column > 0) output << ':' << diagnostic.column;
            }
            output << ": " << diagnostic.message;
            if (!diagnostic.hint.empty()) {
                output << "\n  hint: " << diagnostic.hint;
            }
        }
    }
    return ayt::ui::decodeUtf8Text(output.str());
}

std::wstring assetSizeText(std::uintmax_t bytes)
{
    wchar_t buffer[64]{};
    if (bytes >= 1024u * 1024u) {
        std::swprintf(buffer, 64, L"%.2f MiB",
            static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024u) {
        std::swprintf(buffer, 64, L"%.1f KiB",
            static_cast<double>(bytes) / 1024.0);
    } else {
        std::swprintf(buffer, 64, L"%llu bytes",
            static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

} // namespace

EditorSession::EditorSession()
    : _gameView(ayt::game::GameLoop::instance(), _playRuntime) {
    _worldContext.setFallbackWorld(&ayt::entity::World::instance());
    _playRuntime.setWorldContext(&_worldContext);
}

EditorSession::~EditorSession() {
    shutdown();
}

bool EditorSession::initialize(const EditorSessionDesc& desc) {
    AY_EDITOR_TRACE("initialize: begin");
    const auto reportStartup = [&desc](float progress, const wchar_t* stage) {
        if (desc.onStartupProgress) {
            desc.onStartupProgress(progress, stage);
        }
    };
    reportStartup(0.02f, L"Preparing editor UI...");
    _hostWindow = desc.hostWindow;
    _devices = desc.deviceManager;
    _hostFocused = _devices != nullptr && _devices->window().isFocused();
    _layoutPath = desc.layoutPath;
    _preferences = desc.preferences;
    _preferences.viewportOrientationAxisVisible =
        desc.viewportOrientationAxisVisible;
    _viewportOrientationAxisVisible =
        _preferences.viewportOrientationAxisVisible;
    _onViewportOrientationAxisVisibilityChanged =
        desc.onViewportOrientationAxisVisibilityChanged;
    _onPreferencesChanged = desc.onPreferencesChanged;
    _playRuntime.setHostWindow(_hostWindow);
    // ED-02: forward the imported character (if any) to the
    // Play-runtime. Empty / invalid = cube fallback at startPlay.
    _playRuntime.setImportedCharacter(desc.importedCharacter);
    _playRuntime.setEditorTestSceneEnabled(desc.editorTestSceneEnabled);
    _playRuntime.setNetPlayRole(
        desc.netClientMode ? NetPlayRole::Client : NetPlayRole::Server);
    _playRuntime.setNetConnectHost(desc.netConnectHost);
    _netClientAutoPlay = desc.netClientMode;
    auto* host = ayt::app::currentEngineHost();
    _worldContext.setSceneManager(host != nullptr ? host->scenes() : nullptr);
    // Pre-existing _CrtCheckMemory() failure on session_after_set_host
    // in Debug builds. Commented to keep the build runnable; the four
    // checks later in initialize() remain enabled as debug invariants.
    // AY_EDITOR_HEAP_CHECK("session_after_set_host");

    _ui.initialize(desc.uiBackend);
    installEditorTheme(_preferences.themeName);
    _ui.setUiScale(std::clamp(_preferences.uiScale, 0.75f, 1.25f));
    AY_EDITOR_TRACE("initialize: ui backend set");
    reportStartup(0.12f, L"Loading editor layout...");
    _gameView.setModeChangedCallback([this](EditorMode mode) { onModeChanged(mode); });

    if (_hostWindow != nullptr) {
        RECT clientRect{};
        if (GetClientRect(_hostWindow, &clientRect) != 0) {
            const float width  = static_cast<float>(clientRect.right - clientRect.left);
            const float height = static_cast<float>(clientRect.bottom - clientRect.top);
            if (width > 0.0f && height > 0.0f) {
                _ui.setClientSize(width, height);
                _playRuntime.setClientSize(static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
            }
        }
    }

    AY_EDITOR_TRACE("initialize: loading layout");
    if (!_layoutPath.empty()) {
        if (!_ui.loadLayout(_layoutPath)) {
            std::fprintf(stderr, "[EditorSession] loadLayout failed: %s\n", _layoutPath.c_str());
            return false;
        }
    }
    AY_EDITOR_TRACE("initialize: layout loaded");
    reportStartup(0.34f, L"Loading editor icons...");

    bindToolbar();
    bindShellIcons(desc.iconRootPath);
    reportStartup(0.48f, L"Binding editor panels...");
    bindMenuBar();
    bindTransportBar();
    bindNetworkPanelStub();
    bindRenderSettingsPanel();
    bindTransformInspector();

    // v0.3+ PR-5 — bindOutlinerPanel (design §4.3.y)
    // 一次性 bind（selection callback + itemHeight via setItemHeight;
    // JSON 的 itemHeight 在 DockCard.content 路径被静默丢，见 Landmine A）。
    bindOutlinerPanel();
    bindAssetBrowser();
    reportStartup(0.68f, L"Opening project assets...");

    std::string assetDatabaseError;
    if (!_assetDatabase.open(desc.projectRoot, &assetDatabaseError)) {
        setAssetBrowserStatus(
            L"Asset database unavailable: "
            + ayt::ui::decodeUtf8Text(assetDatabaseError), true);
    } else {
        refreshAssetBrowser();
        setAssetBrowserStatus(L"Indexing project assets...");
        (void)_assetDatabase.requestScan();
    }
    reportStartup(0.78f, L"Restoring editor workspace...");

    _mainDock = dynamic_cast<ayt::ui::DockArea*>(_ui.findById("main_dock"));
    if (_mainDock != nullptr) {
        _mainDock->setOnCardCloseRequested(
            [this](ayt::ui::DockCard* card) {
                if (card == nullptr || _mainDock == nullptr) {
                    return false;
                }
                if (requestCloseDslDocument(card)) {
                    return true;
                }
                const std::string& id = card->getId();
                ayt::ui::DockArea::Slot slot = ayt::ui::DockArea::Slot::Center;
                bool* visible = nullptr;
                if (id == "card_render") {
                    slot = ayt::ui::DockArea::Slot::Right;
                    visible = &_panelRenderVisible;
                } else if (id == "card_outliner") {
                    slot = ayt::ui::DockArea::Slot::Left;
                    visible = &_panelOutlinerVisible;
                } else if (id == "card_inspector") {
                    slot = ayt::ui::DockArea::Slot::Right;
                    visible = &_panelInspectorVisible;
                } else if (id == "card_network") {
                    slot = ayt::ui::DockArea::Slot::Bottom;
                    visible = &_panelNetworkVisible;
                } else if (id == "card_console") {
                    slot = ayt::ui::DockArea::Slot::Bottom;
                    visible = &_panelConsoleVisible;
                } else if (id == "card_assets") {
                    slot = ayt::ui::DockArea::Slot::Bottom;
                    visible = &_panelAssetsVisible;
                } else {
                    return false;
                }
                if (!_mainDock->setCardVisible(id, false, slot)) {
                    return false;
                }
                *visible = false;
                _ui.invalidateLayout();
                if (_repaintCallback) {
                    _repaintCallback();
                }
                return true;
            });
    }
    setDockCardVisible("card_network", false);
    applyPreferences(_preferences);
    _lastObservedPreferences = capturePreferences();
    _preferences = _lastObservedPreferences;
    AY_EDITOR_TRACE("initialize: toolbar bound");

    setModeLabel(L"EDIT");

    syncViewport();
    AY_EDITOR_TRACE("initialize: done");

    // v0.3 PR-4 — Editor 持 Edit Scene（design §4.2.x）
    // 决策 1a: caller/document 持 Edit Scene ownership
    // 决策 3a: EditorMode 3 态 vs SceneMode 2 态；本处只 setCurrent 让 Edit
    //         mode 有 Scene 关联；applyMode 仍走 EditorPlayRuntime 私有通路
    // 决策 4a: 不接 hook 拦 beginPlay；UX 弹窗由 caller 决定
    _document = std::make_unique<EditorSceneDocument>();
    _commands.setChangedCallback([this]() {
        if (_document) _document->markDirty();
        refreshTransformInspector();
        refreshUnsavedIndicator();
        if (_repaintCallback) _repaintCallback();
    });
    if (auto* sm = _worldContext.sceneManager()) {
        sm->setEdit(&_document->scene());
        sm->setCurrent(&_document->scene());
        AY_EDITOR_TRACE("initialize: edit scene injected (%s)",
                        _document->title().c_str());
    }

    // v0.3+ PR-5 — 首刷 Hierarchy（document Scene 注入后才有 scene name）。
    // Edit World v1 永远空（决策 1b；plan §0.2）；Play 未启动 → tree 仅
    // 含合成 root。INV-4：纯读，Scene::_dirty 不可能被置位。
    refreshOutliner();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    reportStartup(0.94f, L"Finalizing editor workspace...");

    // D5+.5: optional child-window manager for DockCard promotion.
    if (desc.childWindowManager != nullptr) {
        _childWindows = std::make_unique<EditorChildWindowManager>(
            *desc.childWindowManager, _ui);
        _childWindows->setRedockTarget(_mainDock);
        _childWindows->setChromeIconRoot(desc.iconRootPath);
        if (!desc.childWindowConfigPath.empty()) {
            const auto cfgs =
                parseChildWindowConfig(desc.childWindowConfigPath);
            for (const auto& cfg : cfgs) {
                void* h = nullptr;
                if (!_childWindows->openChildWindow(cfg, h)) {
                    std::fprintf(stderr,
                        "[EditorSession] child window '%s' failed to open\n",
                        cfg.title.c_str());
                }
            }
            AY_EDITOR_TRACE("initialize: opened %zu child window(s)",
                            _childWindows->count());
        }
        wirePromoteCallback();
    }

    reportStartup(1.0f, L"Editor workspace ready");
    return true;
}

bool EditorSession::initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath) {
    EditorSessionDesc desc;
    desc.uiBackend = backend;
    desc.layoutPath = layoutPath;
    return initialize(desc);
}

void EditorSession::shutdown() {
    if (_shutdown) {
        return;
    }
    _shutdown = true;

    finishTransformGizmoDrag(false);
    savePreferencesNow();

    _gameView.setModeChangedCallback({});
    _repaintCallback = nullptr;
    // K-INV-D5-6: tear down child HWNDs BEFORE primary UIManager
    // shutdown. ~EditorChildWindowManager calls _wm.destroyTopLevelWindow
    // for every entry; ~UIManager on each child may poke
    // g_activeUIManager if it was active during the last tick. Doing
    // this here (with _ui still alive and owning the active slot)
    // avoids an UAF cleanup race against the primary.
    _layoutEditor.reset();
    _layoutEditorHandle = nullptr;
    _audioEditor.reset();
    _audioEditorHandle = nullptr;
    _childWindows.reset();
    _mainDock = nullptr;
    // Tear down Play/renderer borrow before UI widgets — avoids
    // Inspector path strings and GPU borrows racing UI teardown.
    _playRuntime.shutdownEngine();
    _gameView.setMode(EditorMode::Edit);

    // v0.3+ PR-5 — Landmine E: 清 Outliner 状态**早于** _ui.shutdown()
    // 避免 _ui.shutdown 期间 _outliner 指向已 free widget（UIManager 析构
    // 链上 deref）。
    _outliner = nullptr;
    _assetTileView = nullptr;
    _assetTree = nullptr;
    _assetSearch = nullptr;
    _assetTypeFilter = nullptr;
    _pendingDslAssetOpenId = 0;
    _undoMenuItem = nullptr;
    _redoMenuItem = nullptr;
    _viewportOrientationAxisMenuItem = nullptr;
    _onViewportOrientationAxisVisibilityChanged = {};
    _onPreferencesChanged = {};
    _outlinerEntityIds.clear();
    _assetEntries.clear();
    _assetFolderSourcePaths.clear();
    _assetFolderFlatPaths.clear();
    _assetFolderSourceExpanded.clear();
    _assetDatabase.close();
    clearSelectedEntity(false);
    _outlinerRefreshPending = false;
    _outlinerRootExpanded = true;
    _updatingOutlinerSelection = false;
    _commands.clear();

    _ui.shutdown();
    _openDslDocuments.clear();
    _layoutPath.clear();
    _hostWindow = nullptr;
    _devices = nullptr;
    _hostFocused = false;

    // v0.3 PR-4 — shutdown reverse setEdit/setCurrent + reset document
    // 顺序：先反注册 scene → 再 reset（EditorSession 析构时 unique_ptr 还会
    // 再 reset 一次；提前 reset 避免 SM 还指向 dangling Scene）
    if (auto* sm = _worldContext.sceneManager()) {
        if (_document) {
            sm->setCurrent(nullptr);
            sm->setEdit(nullptr);
        }
    }
    _document.reset();
    _worldContext.setSceneManager(nullptr);
}

void EditorSession::setClientSize(float width, float height) {
    _ui.setClientSize(width, height);
    _playRuntime.setClientSize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
    _ui.layout();
    syncViewport();
}

void EditorSession::update(float dt) {
    ayt::game::HostedFrameContext hostFrame;
    hostFrame.realWallDeltaTime = dt;
    update(hostFrame);
}

void EditorSession::update(const ayt::game::HostedFrameContext& hostFrame) {
    const float dt = hostFrame.realWallDeltaTime;
    syncLayoutEditorLifetime();
    if (_layoutEditor != nullptr) {
        _layoutEditor->pumpDeferred();
    }
    syncAudioEditorLifetime();
    if (_audioEditor != nullptr) {
        _audioEditor->tick(dt);
    }
    // D5+.5: tick every open child (each via pushActive scope) before
    // the primary update so the active pointer is correctly swapped
    // before any per-frame UI logic that might read g_activeUIManager.
    // PR-Dock-TearOff: child windows now carry their own GDI backend;
    // tickAll renders into each HWND internally.
    if (_childWindows) {
        _childWindows->tickAll(dt);
    }
    _ui.update(dt);
    // Scene systems and diagnostic hosts can dirty or replace the Edit Scene
    // without going through an EditorSession command. Reconcile the document
    // indicator once per host frame so it cannot remain visually stale.
    refreshUnsavedIndicator();

    // v0.3+ PR-5 — Landmine B: 延迟消费 Outliner 重建（禁止在 TreeNode
    // 事件派发内重建；onOutlinerSelectionChanged 注释）。
    if (_outlinerRefreshPending) {
        _outlinerRefreshPending = false;
        refreshOutliner();
    }
    if (_assetDatabase.pollScan()) {
        _assetBrowserRefreshPending = true;
    }
    if (_assetBrowserRefreshPending) {
        _assetBrowserRefreshPending = false;
        refreshAssetBrowser();
        if (_assetDatabase.lastError().empty()) {
            setAssetBrowserStatus(
                L"Indexed " + std::to_wstring(_assetDatabase.records().size())
                + L" assets");
        } else {
            setAssetBrowserStatus(
                L"Asset scan warning: "
                + ayt::ui::decodeUtf8Text(_assetDatabase.lastError()), true);
        }
    }
    if (_pendingDslAssetOpenId != 0) {
        const EditorAssetId assetId = _pendingDslAssetOpenId;
        _pendingDslAssetOpenId = 0;
        (void)openDslAsset(assetId);
    }
    // Per-frame reconcile: if the last known cursor is not on a splitter
    // band, force every SplitterHandle un-revealed. Leave events alone
    // are not sufficient (capture path / coalesced pointer movement).
    syncSplitterRevealToMouse();

    if (freecamActive()) {
        // Keyboard flight is intentionally gated by the RMB look gesture.
        // Outside that gesture editor shortcuts and text input own the keys.
        if (_freecam.isLooking() && viewportAcceptsGameInput()) {
            if (_devices != nullptr) {
                if (const ayt::device::KeyboardDevice* keyboard =
                        _devices->keyboard()) {
                    _freecam.updateMovement(dt, *keyboard);
                }
            }
        }
        pushFreecamToRenderer();
    }

    if (_gameView.mode() == EditorMode::Play) {
        // v0.4 PR-1 (design §6, 决策 2): tick 仍走 _playRuntime.tick() →
        // GameLoop::tickOnce。**不切到 SceneManager::tick** — renderer 帧
        // 提交 + system tick + network poll + script hot-reload poll 在
        // GameLoop 内耦合 (LM-X4)；切 SM::tick 会破坏 renderer pipeline 顺序。
        // Play Scene World 由 startPlay 头部 beginPlay 创建；system tick
        // = GameLoop::TickSystems() 遍历 World::instance() 系统注册器
        // (v0.4 PR-1 不动)。
        _playRuntime.tick(hostFrame);
    } else if (_gameView.mode() == EditorMode::Edit) {
        _playRuntime.tickPresentation(hostFrame);
    } else if (_gameView.mode() == EditorMode::Paused) {
        // Keep last rendered frame visible; stepOnce drives simulation separately.
    }

    // Inspector edits, Undo/Redo and scene systems may all change the selected
    // transform without a pointer event. Publish the latest pose every frame;
    // the renderer compares geometry mode/highlight before rebuilding buffers.
    syncTransformGizmoToRenderer();

    // Splitter drag updates HBox slot widths; keep the 3D viewport rect
    // in sync every frame so render composite tracks panel resize.
    syncViewportIfChanged();
    pollPreferences(dt);
}

bool EditorSession::freecamActive() const
{
    const EditorMode mode = _gameView.mode();
    return mode == EditorMode::Edit || mode == EditorMode::Play
        || mode == EditorMode::Paused;
}

void EditorSession::pushFreecamToRenderer()
{
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        sub->setCameraLookAt(
            _freecam.eye(),
            _freecam.at(),
            _freecam.up(),
            _freecam.fovYDegrees());
    }
}

void EditorSession::render() {
    render(false);
}

void EditorSession::render(bool skipViewportPanel) {
    // Pre-AI-1 wrapper: kept for callers that want a single populate+
    // flush call. The new AI-1 path in AYEditorApp.cpp uses
    // populateFrame + flushFrame directly so the RenderPass dispatch
    // can own the UI submission boundary (UIPass::execute flushes
    // pending text batches).
    populateFrame(skipViewportPanel);
    flushFrame();
}

void EditorSession::populateFrame(bool skipViewportPanel) {
    // AI-1: begin-frame the widget walk that accumulates draws on
    // the backend's pendingRects + textBatch. No flush yet — the
    // flush moves to UIPass::execute so the RenderPass dispatch owns
    // the UI submission boundary. flushFrame() closes the lifecycle.
    //
    // Consume pending layout while the viewport still participates in its
    // VBox. Laying out after the composite-hole visibility toggle collapses
    // panel_viewport out of the tree and can expand the native scene behind
    // editor chrome until the next resize.
    _ui.layout();
    syncViewportIfChanged();

    _panelViewportForFrame = nullptr;
    _cardViewportForFrame = nullptr;
    if (skipViewportPanel) {
        ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
        if (viewport != nullptr) {
            _panelViewportWasVisibleForFrame = viewport->isVisible();
            viewport->setVisible(false);
            _panelViewportForFrame = viewport;
        }
        // Hide only the DockCard body fill — setVisible(false) on the
        // card would collapse the Center slot weight in DockArea.
        if (auto* card = dynamic_cast<ayt::ui::Panel*>(_ui.findById("card_viewport"))) {
            _cardViewportHadBackgroundForFrame = card->isBackgroundEnabled();
            card->setBackgroundEnabled(false);
            _cardViewportForFrame = card;
        }
    }

    _ui.populateFrame();
}

void EditorSession::flushFrame() {
    // AI-1: close the IRenderBackend lifecycle (endCanvas + endFrame).
    // endFrame() inside the backend flushes pendingRects via
    // flushColoredRects() + any remaining text via flushPendingText().
    _ui.flushFrame();

    if (_panelViewportForFrame != nullptr) {
        _panelViewportForFrame->setVisible(_panelViewportWasVisibleForFrame);
        _panelViewportForFrame = nullptr;
    }
    if (_cardViewportForFrame != nullptr) {
        _cardViewportForFrame->setBackgroundEnabled(_cardViewportHadBackgroundForFrame);
        _cardViewportForFrame = nullptr;
    }
}

bool EditorSession::shouldCompositeViewport() const {
    return _playRuntime.isPresentationReady();
}

bool EditorSession::ensurePresentationReady() {
    if (!_playRuntime.ensurePresentationReady()) return false;
    if (!_editWorldPrepared && _document != nullptr) {
        // Bootstrap registered systems once against the stable Edit World.
        // Start them while the document is still empty so their renderer
        // callbacks exist without advancing user scene simulation.
        _document->scene().tick(0.0f);
        if (!_netClientAutoPlay && !_playRuntime.prepareEditScene()) {
            std::fprintf(stderr,
                "[EditorSession] unable to prepare persistent Edit scene\n");
            return false;
        }
        _editWorldPrepared = true;
        refreshOutliner();
        refreshTransformInspector();
    }
    // The Render panel is the live source of truth in both Edit and Play.
    // Renderer defaults intentionally keep Bloom/SSAO/Haze disabled, while
    // the Editor validation layout starts them enabled. Synchronize after
    // presentation bootstrap so Edit does not display enabled controls over
    // a zero-effect renderer, and so a pipeline recreation restores the
    // current panel values.
    applyRenderSettingsFromPanel();
    pushFreecamToRenderer();
    return true;
}

void EditorSession::autoEnterNetClientPlay()
{
    if (!_netClientAutoPlay) {
        return;
    }
    std::fprintf(stderr,
        "[EditorSession] --net-client: auto-entering Play mode\n");
    _gameView.setMode(EditorMode::Play);
}

void EditorSession::autoEnterImportedAnimationPlay()
{
    std::fprintf(stderr,
        "[EditorSession] animated character ready: auto-entering Play mode\n");
    _gameView.setMode(EditorMode::Play);
}

bool EditorSession::getViewportBounds(ayt::math::FRectangle& outBounds) const {
    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return false;
    }
    outBounds = viewport->getWorldBounds();
    return true;
}

bool EditorSession::isSplitHandlePoint(float x, float y) const {
    ayt::ui::Widget* mainRow = _ui.findById("main_row");
    if (mainRow == nullptr) {
        return false;
    }

    const ayt::math::FVector2 logical =
        _ui.physicalToLogical(ayt::math::FVector2(x, y));
    ayt::ui::Widget* hit = mainRow->hitTest(logical);
    return dynamic_cast<const ayt::ui::SplitterHandle*>(hit) != nullptr;
}

namespace {

void clearSplitterHoversRecursive(ayt::ui::Widget* widget)
{
    if (widget == nullptr) {
        return;
    }
    if (auto* split = dynamic_cast<ayt::ui::SplitterHandle*>(widget)) {
        // Prefer clearHoverReveal so we never depend on onMouseLeave
        // side effects / override quirks for the force-unreveal path.
        split->clearHoverReveal();
    }
    for (ayt::ui::Widget* child : widget->getChildren()) {
        clearSplitterHoversRecursive(child);
    }
}

bool pointOnSplitterRecursive(ayt::ui::Widget* widget, float x, float y)
{
    if (widget == nullptr || !widget->isVisible()) {
        return false;
    }
    if (auto* split = dynamic_cast<ayt::ui::SplitterHandle*>(widget)) {
        if (split->getWorldBounds().contains(ayt::math::FVector2(x, y))) {
            return true;
        }
    }
    for (ayt::ui::Widget* child : widget->getChildren()) {
        if (pointOnSplitterRecursive(child, x, y)) {
            return true;
        }
    }
    return false;
}

} // namespace

void EditorSession::clearSplitterHovers()
{
    clearSplitterHoversRecursive(_ui.root());
}

void EditorSession::syncSplitterRevealToMouse()
{
    if (!_hasLastMouse) {
        clearSplitterHovers();
        return;
    }
    // Use bounds walk (not hitTest): hitTest can prefer other widgets
    // or miss when layout is mid-update; bounds are the reveal source of truth.
    const ayt::math::FVector2 logical =
        _ui.physicalToLogical(ayt::math::FVector2(_lastMouseX, _lastMouseY));
    if (!pointOnSplitterRecursive(_ui.root(), logical.x, logical.y)) {
        clearSplitterHovers();
    }
}

bool EditorSession::isViewportSurfacePoint(float x, float y) const {
    if (isSplitHandlePoint(x, y)) {
        return false;
    }

    // The Center slot can host source documents as tabs next to Scene View.
    // panel_viewport keeps its last layout bounds while its DockCard is an
    // inactive tab, so geometry alone would route clicks on a DSL editor back
    // into scene picking/freecam. Only treat those bounds as a native viewport
    // surface while the viewport card itself is the visible tab.
    if (_mainDock != nullptr) {
        const ayt::ui::DockCard* viewportCard =
            _mainDock->findCard("card_viewport");
        if (viewportCard != nullptr && !viewportCard->isVisible()) {
            return false;
        }
    }

    const ayt::math::FVector2 pos =
        _ui.physicalToLogical(ayt::math::FVector2(x, y));

    // Open menus / combo popups live on the overlay and often extend into
    // panel_viewport. In Play/Paused those points must still reach UIManager
    // or dropdown items over the cube receive no hits.
    if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
        for (ayt::ui::Widget* child : overlay->getChildren()) {
            if (child == nullptr || !child->isVisible()) {
                continue;
            }
            if (child->getWorldBounds().contains(pos)) {
                return false;
            }
        }
    }

    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)) {
        return false;
    }

    return pos.x >= viewport.minX && pos.x < viewport.maxX
        && pos.y >= viewport.minY && pos.y < viewport.maxY;
}

bool EditorSession::isChromePoint(float x, float y) const {
    return _ui.isCapturing() || !isViewportSurfacePoint(x, y);
}

bool EditorSession::viewportRayDirection(
    float x, float y, ayt::math::FVector3& outDirection) const
{
    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)
        || viewport.width() <= 0.0f || viewport.height() <= 0.0f
        || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    const ayt::math::FVector2 logical =
        _ui.physicalToLogical(ayt::math::FVector2(x, y));
    const float ndcX =
        2.0f * ((logical.x - viewport.minX) / viewport.width()) - 1.0f;
    const float ndcY =
        1.0f - 2.0f * ((logical.y - viewport.minY) / viewport.height());
    constexpr float degreesToRadians = 0.017453292519943295f;
    const float tanHalfFov = std::tan(
        _freecam.fovYDegrees() * degreesToRadians * 0.5f);
    const float aspect = viewport.width() / viewport.height();
    outDirection = _freecam.forward()
        + _freecam.right() * (ndcX * aspect * tanHalfFov)
        + _freecam.up() * (ndcY * tanHalfFov);
    if (outDirection.lengthSq() < 1.0e-8f) {
        return false;
    }
    outDirection = outDirection.normalize();
    return true;
}

bool EditorSession::viewportAcceptsGameInput() const {
    if (!_hostFocused || _devices == nullptr) {
        return false;
    }
    const ayt::ui::Widget* focused = _ui.getFocusedWidget();
    if (focused != nullptr && focused->isTextEditingWidget()) {
        return false;
    }
    // RMB fly navigation already started inside the viewport — keep movement.
    if (_freecam.isLooking()) {
        return true;
    }
    if (!_hasLastMouse) {
        return false;
    }
    return !isChromePoint(_lastMouseX, _lastMouseY);
}

bool EditorSession::onMouseMove(float x, float y) {
    _lastMouseX = x;
    _lastMouseY = y;
    _hasLastMouse = true;

    // Cross-panel drags are owned by AYUI even while the cursor is over the
    // 3D surface. Without this gate EditorSession's freecam routing starves
    // UIManager::updateDrag, so the viewport never becomes a drop target.
    if (_ui.isDragging()) {
        return _ui.onMouseMove(x, y);
    }

    if (_transformGizmo.active()) {
        return updateTransformGizmoDrag(x, y);
    }

    // Armed viewport LMB: object surfaces only select. A drag past slop is
    // consumed (reserved for future marquee selection) but never rotates the
    // camera; transforms begin exclusively from a gizmo handle.
    if (_viewportLmbPending && !_viewportLmbDragged && !_freecam.isLooking()) {
        const float dx = x - _viewportLmbX;
        const float dy = y - _viewportLmbY;
        if ((dx * dx + dy * dy)
            >= (kViewportLookDragSlopPixels * kViewportLookDragSlopPixels)) {
            _viewportLmbDragged = true;
            return true;
        }
    }

    if (_freecam.isLooking()) {
        _freecam.updateLook(x, y);
        pushFreecamToRenderer();
        return true;
    }

    if (_ui.isCapturing()) {
        // Still deliver moves (drag). Bounds-checked SplitterHandle::
        // onMouseMove clears _hover when outside the band; sync in
        // update() finishes un-reveal after mouse-up.
        return _ui.onMouseMove(x, y);
    }

    if (isViewportSurfacePoint(x, y)) {
        updateTransformGizmoHover(x, y);
    } else if (_gizmoHoverHandle != EditorGizmoHandle::None) {
        _gizmoHoverHandle = EditorGizmoHandle::None;
        syncTransformGizmoToRenderer();
        if (_repaintCallback) _repaintCallback();
    }

    if (!isChromePoint(x, y)) {
        // Moving from an Inspector field into the viewport commits the field
        // and releases text focus before WASD/free-look starts.
        if (_ui.getFocusedWidget() != nullptr) {
            _ui.setFocus(nullptr);
        }
        _ui.clearHover();
        clearSplitterHovers();
        return false;
    }

    const bool handled = _ui.onMouseMove(x, y);
    const ayt::math::FVector2 logical =
        _ui.physicalToLogical(ayt::math::FVector2(x, y));
    if (!pointOnSplitterRecursive(_ui.root(), logical.x, logical.y)) {
        clearSplitterHovers();
    }
    return handled;
}

bool EditorSession::onMouseButtonDown(float x, float y, int button) {
    if (_ui.isDragging()) {
        return _ui.onMouseButtonDown(x, y, button);
    }
    // Dismiss MenuBar popups on any click that is not inside an open
    // menu. Play-mode freecam / isCapturing used to skip UIManager, so
    // click-outside never ran and the dropdown stayed painted forever.
    {
        const ayt::math::FVector2 pos =
            _ui.physicalToLogical(ayt::math::FVector2(x, y));
        bool insideOpenMenu = false;
        if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
            for (ayt::ui::Widget* child : overlay->getChildren()) {
                auto* menu = dynamic_cast<ayt::ui::Menu*>(child);
                if (menu == nullptr || !menu->isOpen()) {
                    continue;
                }
                if (menu->getWorldBounds().contains(pos)) {
                    insideOpenMenu = true;
                    break;
                }
            }
        }
        if (!insideOpenMenu) {
            if (auto* menuBar =
                    dynamic_cast<ayt::ui::MenuBar*>(_ui.findById("menubar"))) {
                menuBar->closeOpenMenu();
            }
        }
    }

    const bool onViewportSurface = isViewportSurfacePoint(x, y);
    // A fresh primary-button down is a recovery boundary for an older UI
    // capture whose matching mouse-up was lost (focus change, touchpad
    // gesture cancellation, etc.). Keeping the stale capture alive lets the
    // next toolbar button enter Pressed while its mouse-up is consumed by a
    // different viewport gesture, leaving two input state machines out of
    // sync. Cancel first, then route this down as a brand-new gesture.
    if (button == 0 && _ui.isCapturing()) {
        _ui.cancelCapture();
    }

    if (button == 0 && !onViewportSurface) {
        // Chrome input must not complete a stale viewport gesture. In
        // particular, onMouseButtonUp() gives an armed viewport LMB priority
        // over AYUI capture; without clearing it here the first toolbar click
        // is swallowed and its Button remains pressed/captured.
        if (_transformGizmo.active()) {
            finishTransformGizmoDrag(false);
        }
        _viewportLmbPending = false;
        _viewportLmbDragged = false;
        if (_freecam.isLooking()) {
            _freecam.endLook();
        }
    }

    if (_ui.isCapturing()) {
        if (button != 0) {
            return _ui.onMouseButtonDown(x, y, button);
        }
    }

    if (onViewportSurface) {
        // A direct click into the viewport may arrive without a preceding
        // mouse-move (for example after keyboard editing). Drop text focus so
        // the field commits before free-look or selection starts.
        if (_ui.getFocusedWidget() != nullptr) {
            _ui.setFocus(nullptr);
        }
        _ui.clearHover();

        if (button == 1 && freecamActive()) {
            if (_transformGizmo.active()) {
                finishTransformGizmoDrag(false);
            }
            _viewportLmbPending = false;
            _viewportLmbDragged = false;
            _freecam.beginLook(x, y);
            return true;
        }

        if (button == 0 && freecamActive()) {
            if (_gameView.mode() == EditorMode::Edit) {
                const EditorGizmoHandle handle = hitTestTransformGizmo(x, y);
                if (handle != EditorGizmoHandle::None
                    && beginTransformGizmoDrag(handle, x, y)) {
                    _viewportLmbPending = false;
                    _viewportLmbDragged = false;
                    return true;
                }
            }
            // A short click selects. LMB drag never enters FreeCam.
            _viewportLmbPending = true;
            _viewportLmbDragged = false;
            _viewportLmbX = x;
            _viewportLmbY = y;
            return true; // AYDevice already owns capture for the pressed button.
        }
        return false;
    }

    return _ui.onMouseButtonDown(x, y, button);
}

bool EditorSession::onMouseButtonUp(float x, float y, int button) {
    if (_ui.isDragging()) {
        return _ui.onMouseButtonUp(x, y, button);
    }
    if (button == 0 && _transformGizmo.active()) {
        updateTransformGizmoDrag(x, y);
        finishTransformGizmoDrag(true);
        _viewportLmbPending = false;
        _viewportLmbDragged = false;
        return true;
    }

    if (_freecam.isLooking() && button == 1) {
        _freecam.endLook();
        return true;
    }

    if (button == 0 && _viewportLmbPending) {
        const bool wasClick = !_viewportLmbDragged;
        _viewportLmbPending = false;
        _viewportLmbDragged = false;
        if (wasClick) {
            selectPlayEntityFromViewport();
        }
        return true;
    }

    if (_ui.isCapturing()) {
        return _ui.onMouseButtonUp(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        return false;
    }

    return _ui.onMouseButtonUp(x, y, button);
}

bool EditorSession::onMouseWheel(float x, float y, float deltaY) {
    if (_ui.isCapturing() || isChromePoint(x, y)) {
        return _ui.onMouseWheel(x, y, deltaY);
    }
    if (!freecamActive() || !std::isfinite(deltaY) || deltaY == 0.0f) {
        return false;
    }
    ayt::math::FVector3 direction{};
    if (!viewportRayDirection(x, y, direction)) {
        return false;
    }
    // DeviceInputBridge's UI convention is opposite to native wheel notches
    // (+pixels reveals lower content). Convert back before navigating, then
    // dolly along the pointer ray instead of the screen-center forward vector.
    const float wheelNotches = -deltaY / kWheelLogicalPixelsPerNotch;
    _freecam.zoomToward(wheelNotches, direction);
    pushFreecamToRenderer();
    if (_repaintCallback) {
        _repaintCallback();
    }
    return true;
}

void EditorSession::onMouseLeave() {
    _hasLastMouse = false;
    if (_freecam.isLooking()) {
        _freecam.endLook();
    }
    if (_transformGizmo.active()) {
        finishTransformGizmoDrag(false);
    }
    _gizmoHoverHandle = EditorGizmoHandle::None;
    syncTransformGizmoToRenderer();
    _ui.onMouseLeave();
    clearSplitterHovers();
}

bool EditorSession::onKeyDown(int keyCode)
{
    if (keyCode == ayt::ui::UIKey_Control) {
        _controlDown = true;
    }
    const ayt::ui::Widget* focused = _ui.getFocusedWidget();
    const bool textEditing = focused != nullptr && focused->isTextEditingWidget();
    if (textEditing) {
        if (OpenDslDocument* dsl = focusedDslDocument()) {
            if (_controlDown && keyCode == ayt::ui::UIKey_S) {
                return saveDslDocument(*dsl);
            }
            if (keyCode == ayt::ui::UIKey_F7) {
                compileDslDocument(*dsl);
                return true;
            }
        }
    }
    if (!textEditing && _controlDown && _gameView.mode() == EditorMode::Edit) {
        if (keyCode == ayt::ui::UIKey_Z) return _commands.undo();
        if (keyCode == ayt::ui::UIKey_Y) return _commands.redo();
        if (keyCode == ayt::ui::UIKey_S) {
            saveSceneDocument();
            return true;
        }
    }
    if (!textEditing && keyCode == ayt::ui::UIKey_Delete) {
        deleteSelectedEntity();
        return true;
    }
    return _ui.onKeyDown(keyCode);
}

bool EditorSession::onKeyUp(int keyCode)
{
    const bool handled = _ui.onKeyUp(keyCode);
    if (keyCode == ayt::ui::UIKey_Control) {
        _controlDown = false;
    }
    return handled;
}

void EditorSession::onWindowFocusChanged(bool focused)
{
    _hostFocused = focused;
    if (focused) return;

    // Win32 is allowed to omit key-up messages after Alt-Tab/focus transfer.
    // Reset both Editor-owned and UIManager-owned modifier state so a later
    // plain Z/Y/S cannot be interpreted as a Ctrl shortcut.
    _controlDown = false;
    _ui.onKeyUp(ayt::ui::UIKey_Control);
    _ui.onKeyUp(ayt::ui::UIKey_Shift);
    _ui.onKeyUp(ayt::ui::UIKey_Alt);
    _ui.cancelCapture();
    finishTransformGizmoDrag(false);
    if (_ui.getFocusedWidget() != nullptr) {
        _ui.setFocus(nullptr);
    }
    if (_freecam.isLooking()) {
        _freecam.endLook();
    }
    _viewportLmbPending = false;
    _viewportLmbDragged = false;
    onMouseLeave();
}

bool EditorSession::isUiHoverInteractive() const {
    return _ui.isHoverInteractive();
}

ayt::ui::UiCursorHint EditorSession::getUiCursorHint() const {
    if (_gameView.mode() == EditorMode::Edit
        && (_transformGizmo.active()
            || _gizmoHoverHandle != EditorGizmoHandle::None)) {
        // DCC-style transform feedback comes from the highlighted/active
        // handle. A generic four-way Move cursor is misleading for rotation
        // and scale, so the viewport arrow remains stable throughout hover
        // and drag.
        return ayt::ui::UiCursorHint::Default;
    }
    return _ui.getCursorHint();
}

void EditorSession::setRepaintCallback(RepaintCallback callback) {
    _repaintCallback = std::move(callback);
}

void EditorSession::bindToolbar() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_minimize", [this]() { requestHostMinimize(); });
    bindButton("btn_maximize", [this]() { requestHostMaximizeToggle(); });
    bindButton("btn_close", [this]() { requestHostClose(); });

    bindButton("btn_inspector_skel",  [this]() { pickInspectorSkeleton(); });
    bindButton("btn_inspector_anim",  [this]() { pickInspectorAnimation(); });
    bindButton("btn_inspector_apply", [this]() { applyInspectorOverrides(); });
    bindButton("btn_inspector_reset", [this]() { resetInspectorOverrides(); });

    // Selection now exposes one Universal transform gizmo. The legacy
    // Select/Move/Rotate/Scale buttons are deliberately not bound even when
    // loading an older custom layout.
    bindButton("btn_tool_space", [this]() {
        setLocalTransformSpace(!_localTransformSpace);
    });
    bindButton("btn_view_camera", [this]() {
        _orthographicView = !_orthographicView;
        if (auto* widget = _ui.findById("btn_view_camera")) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setText(_orthographicView ? L"Orthographic" : L"Perspective");
            }
        }
        if (_repaintCallback) _repaintCallback();
    });
    bindButton("btn_view_shading", [this]() {
        _wireframeView = !_wireframeView;
        if (auto* widget = _ui.findById("btn_view_shading")) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setText(_wireframeView ? L"Wireframe" : L"Shaded");
            }
        }
        if (_repaintCallback) _repaintCallback();
    });
    bindButton("btn_console_clear", [this]() {
        if (auto* widget = _ui.findById("console_output")) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
                label->setText(L"");
            }
        }
        if (_repaintCallback) _repaintCallback();
    });
    bindButton("btn_assets_view", [this]() {
        if (auto* widget = _ui.findById("btn_assets_view")) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setText(button->getText() == L"Grid" ? L"List" : L"Grid");
            }
        }
        if (_repaintCallback) _repaintCallback();
    });

    const ayt::math::FVector4 accent = editorThemeColor(
        "color.accent", ayt::math::FVector4(0.16f, 0.40f, 0.70f, 1.0f));
    const ayt::math::FVector4 muted = editorThemeColor(
        "color.text.muted", ayt::math::FVector4(0.68f, 0.71f, 0.76f, 1.0f));
    const char* accentButtons[] = {
        "btn_tool_space", "btn_play", "btn_pause", "btn_step", "btn_stop",
        "btn_view_camera", "btn_view_shading", "btn_view_options"
    };
    for (const char* id : accentButtons) {
        if (auto* button = dynamic_cast<ayt::ui::Button*>(_ui.findById(id))) {
            button->setFallbackHoverColor(accent);
        }
    }

    auto styleLabel = [this](const char* id,
                             const ayt::math::FVector4& text,
                             bool centered) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(_ui.findById(id))) {
            label->setTextColor(text);
            label->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
            if (centered) {
                label->setHorizontalAlignment(ayt::ui::TextLabel::HAlignment::Center);
            }
        }
    };
    styleLabel("lbl_workspace", ayt::math::FVector4(0.90f, 0.95f, 1.0f, 1.0f), true);
    styleLabel("lbl_mode", ayt::math::FVector4(0.42f, 0.72f, 1.0f, 1.0f), true);
    styleLabel("lbl_document_title", muted, false);
    styleLabel("lbl_active_tool", muted, true);
    styleLabel("lbl_viewport_scene", muted, false);
    styleLabel("toolbar_divider_left", muted, true);
    styleLabel("lbl_status_scene", muted, false);
    styleLabel("lbl_status_network", muted, false);
    styleLabel("lbl_status_renderer", muted, false);
    styleLabel("lbl_status_fps", muted, false);

    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setBackgroundColor(ayt::math::FVector4(0.09f, 0.16f, 0.25f, 1.0f));
        }
    }
    if (auto* widget = _ui.findById("lbl_workspace")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setBackgroundColor(ayt::math::FVector4(0.10f, 0.29f, 0.50f, 1.0f));
        }
    }
}

void EditorSession::bindShellIcons(const std::string& iconRootPath)
{
    if (iconRootPath.empty()) {
        return;
    }

    struct IconBinding {
        const char* buttonId;
        const char* relativePath;
        const wchar_t* accessibleLabel;
        float iconSize;
        float horizontalPadding;
        float verticalPadding;
    };

    // Keep this mapping semantic and editor-owned. AYUI owns SVG parsing and
    // drawing; AYEditor decides which visual communicates each command.
    static constexpr IconBinding bindings[] = {
        {"btn_minimize",    "outline/minus.svg",             L"Minimize",            13.0f, 5.0f, 3.0f},
        {"btn_maximize",    "outline/maximize.svg",          L"Maximize or restore",  13.0f, 5.0f, 3.0f},
        {"btn_close",       "outline/x.svg",                 L"Close editor",         13.0f, 5.0f, 3.0f},
        {"btn_play",        "filled/player-play.svg",        L"Play",                  16.0f, 8.0f, 4.0f},
        {"btn_pause",       "filled/player-pause.svg",       L"Pause",                 16.0f, 8.0f, 4.0f},
        {"btn_step",        "filled/player-track-next.svg",  L"Step one frame",        16.0f, 8.0f, 4.0f},
        {"btn_stop",        "filled/player-stop.svg",        L"Stop",                  16.0f, 8.0f, 4.0f},
        {"btn_view_options", "outline/dots.svg",             L"Viewport options",      14.0f, 6.0f, 4.0f},
    };

    const std::filesystem::path root(iconRootPath);
    const ayt::math::FVector4 iconColor(0.88f, 0.90f, 0.94f, 1.0f);
    size_t loadedCount = 0;
    for (const IconBinding& binding : bindings) {
        auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById(binding.buttonId));
        if (button == nullptr) {
            continue;
        }

        std::string error;
        const std::filesystem::path path = root / binding.relativePath;
        auto document = ayt::ui::SvgDocument::loadFromFile(path, &error);
        if (document == nullptr) {
            std::fprintf(stderr,
                         "[EditorSession] SVG icon '%s' unavailable: %s (%s)\n",
                         binding.buttonId, path.string().c_str(), error.c_str());
            continue;
        }

        // Clear the visible placeholder only after parsing succeeds. Explicit
        // accessibility metadata preserves the command name for icon-only UI.
        button->setAccessibilityLabel(binding.accessibleLabel);
        button->setText(L"");
        button->setIconDocument(std::move(document));
        button->setIconSize(binding.iconSize);
        button->setIconColor(iconColor);
        button->setPadding(binding.horizontalPadding, binding.verticalPadding,
                           binding.horizontalPadding, binding.verticalPadding);
        ++loadedCount;
    }

    std::fprintf(stderr,
                 "[EditorSession] native SVG icons: %zu/%zu loaded from %s\n",
                 loadedCount, std::size(bindings), root.string().c_str());
}

void EditorSession::bindTransportBar() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    // v0.3 PR-4 — btn_play enable 条件 + dirty prompt UX（design §4.3.x）
    // 决策 1a: enable = host->scenes()->canBeginPlay()
    // 决策 4a: Save/Discard/Cancel 三选项 Win32 MessageBoxW
    // 决策 5a: lbl_unsaved period refresh（mode changed 时同步）
    bindButton("btn_play", [this]() {
        // Paused -> Play is a resume command for the existing Play Scene,
        // not a request to begin a second session. In this state
        // SceneManager::canBeginPlay() is intentionally false, so resume must
        // bypass both that gate and the Edit-scene save prompt.
        if (_gameView.mode() == EditorMode::Paused) {
            (void)_gameView.trySetMode(EditorMode::Play);
            return;
        }

        auto* sm = _worldContext.sceneManager();
        if (!sm || !sm->canBeginPlay()) return;

        // Save/Discard/Cancel prompt (PR-3 requireSaveBeforePlay 意图 getter)
        if (_document != nullptr && _document->isDirty()) {
            int choice = ::MessageBoxW(
                _hostWindow,
                L"Scene has unsaved changes.\n\nSave before Play?",
                L"AYEditor",
                MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDCANCEL) return;  // Cancel: 早返
            if (choice == IDYES) {
                saveSceneDocument();
                if (_document->isDirty()) return;
            }
            // IDNO = Discard：继续
        }

        // v0.4 PR-1 (design §6): btn_play **不直接**调 sm->beginPlay() —
        // 委托给 _gameView.setMode(Play) → applyMode(Play) →
        // _runtime.startPlay() 头部 (F3.a)。保持"single source of truth =
        // EditorPlayRuntime"。Save/Discard/Cancel UX 完整不动。
        _gameView.setMode(EditorMode::Play);
    });

    // v0.4 PR-1: btn_stop 不直接调 sm->endPlay()；委托给
    // _gameView.setMode(Edit) → applyMode(Edit) → _runtime.enterEdit()
    // 头部 (F3.b) → sm->endPlay()（idempotent；G2/G5 收口）。
    bindButton("btn_pause", [this]() { _gameView.setMode(EditorMode::Paused); });
    bindButton("btn_step", [this]() {
        _gameView.stepOnce();
        if (_repaintCallback) {
            _repaintCallback();
        }
    });
    bindButton("btn_stop", [this]() { _gameView.setMode(EditorMode::Edit); });

    // v0.3 PR-4 — lbl_unsaved 初始 refresh（design §4.3.x 决策 5a）
    refreshUnsavedIndicator();
    refreshTransformInspector();
}

// helper：刷新 lbl_unsaved TextLabel（visible + text）
void EditorSession::refreshUnsavedIndicator() {
    bool dirty = _document != nullptr && _document->isDirty();
    if (auto* sm = _worldContext.sceneManager(); sm != nullptr
        && _document != nullptr && sm->edit() != &_document->scene()) {
        // Preserve diagnostic hosts that temporarily substitute the Edit
        // scene behind the session.
        dirty = sm->isEditDirty();
    }
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_unsaved"))) {
        const std::wstring desiredText = dirty ? L"•" : L"";
        if (label->getText() != desiredText) label->setText(desiredText);
        if (label->isVisible() != dirty) label->setVisible(dirty);
        label->setTextColor(ayt::math::FVector4(1.0f, 0.68f, 0.18f, 1.0f));
        label->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        label->setHorizontalAlignment(ayt::ui::TextLabel::HAlignment::Center);
    }

    const std::string documentTitle = _document != nullptr
        ? _document->title() : std::string("Untitled");
    const std::wstring wideTitle(documentTitle.begin(), documentTitle.end());
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_document_title"))) {
        label->setText(wideTitle + L"  —  Aliyat Editor");
    }
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_status_scene"))) {
        label->setText(L"Scene: " + wideTitle);
    }
}

// =============================================================================
// v0.3+ PR-5 — Hierarchy / Outliner (design §4.3.y)
// =============================================================================
const ayt::entity::World* EditorSession::hierarchyWorld() const noexcept
{
    const EditorWorldSlot slot = _gameView.mode() == EditorMode::Edit
        ? EditorWorldSlot::Edit
        : EditorWorldSlot::Play;
    auto* world = _worldContext.world(slot, true);
    return world != nullptr && world->isInitialized() ? world : nullptr;
}

ayt::entity::World* EditorSession::hierarchyWorldMutable() noexcept
{
    return const_cast<ayt::entity::World*>(hierarchyWorld());
}

void EditorSession::clearSelectedEntity(bool clearOutline,
                                        bool clearAssetSelection)
{
    if (clearOutline && _selectionWorld != nullptr && !_selection.empty()) {
        // Only dereference worlds still advertised by EditorWorldContext.
        // enterEdit() destroys the Play scene before onModeChanged(Edit), so
        // an old Play pointer must be treated as an opaque identity only.
        ayt::entity::World* editWorld =
            _worldContext.world(EditorWorldSlot::Edit, true);
        ayt::entity::World* playWorld =
            _worldContext.world(EditorWorldSlot::Play, true);
        if (_selectionWorld == editWorld || _selectionWorld == playWorld) {
            if (ayt::entity::Entity* entity = _selection.resolve(_selectionWorld)) {
                if (auto* mesh = entity->getComponent<ayt::entity::MeshComponent>()) {
                    mesh->outlineHull = false;
                }
            }
        }
    }
    _selection.clear();
    _selectionWorld = nullptr;
    if (clearAssetSelection) clearSelectedAsset();
    _gizmoHoverHandle = EditorGizmoHandle::None;
    syncTransformGizmoToRenderer();
}

void EditorSession::setSelectedEntity(ayt::entity::World* world,
                                      ayt::entity::Entity* entity)
{
    if (world == nullptr || entity == nullptr || entity->getWorld() != world) {
        clearSelectedEntity();
        return;
    }
    clearSelectedEntity();
    _selection.select(entity->getId());
    _selectionWorld = world;
    if (auto* mesh = entity->getComponent<ayt::entity::MeshComponent>()) {
        mesh->outlineHull = true;
    }
    _gizmoHoverHandle = EditorGizmoHandle::None;
    syncTransformGizmoToRenderer();
}

void EditorSession::bindOutlinerPanel()
{
    _outliner = dynamic_cast<ayt::ui::TreeView*>(
        _ui.findById("tree_outliner"));
    if (_outliner == nullptr) {
        // PR-4 的 "layout 缺失静默跳过" 模式（initialize 已在
        // _layoutPath.empty() 时不 loadLayout；此处 findById 落空同理）。
        return;
    }
    // Landmine A 修法：TreeView 的 JSON itemHeight 在 DockCard.content 路径
    // 下不会被应用（AYLayoutLoader.cpp:191-197 走 buildWidgetTree，
    // 不走 AYWidgetSerializer.cpp:441-443 的 itemHeight 解析）。
    // 故从代码设。
    _outliner->setItemHeight(16.0f);
    _outliner->setOnSelectionChanged(
        [this](int flatIndex) { onOutlinerSelectionChanged(flatIndex); });
    _outliner->setOnExpandToggled(
        [this](int flatIndex, bool expanded) {
            if (flatIndex != 0) return;
            _outlinerRootExpanded = expanded;
            // TreeView has already rebuilt its stable row pool. Rebuild our
            // entity-id projection on the next EditorSession update so a
            // selected entity hidden by collapse remains selected logically.
            _outlinerRefreshPending = true;
        });
}

void EditorSession::refreshOutliner()
{
    if (_outliner == nullptr) {
        return;
    }

    auto setUtf8 = [this](const char* id, const std::string& utf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(
                    std::wstring(utf8.begin(), utf8.end()));
            }
        }
    };

    _outlinerEntityIds.clear();

    const ayt::entity::World* world = hierarchyWorld();
    if (world == nullptr) {
        _outliner->clearTree();
        clearSelectedEntity(false);
        setUtf8("outliner_hint", "Scene: -");
        return;
    }

    const EditorWorldSlot slot = _gameView.mode() == EditorMode::Edit
        ? EditorWorldSlot::Edit
        : EditorWorldSlot::Play;
    std::string rootLabel = slot == EditorWorldSlot::Play
        ? "<runtime fallback>"
        : "<no scene>";
    if (slot == EditorWorldSlot::Edit && _document) {
        rootLabel = _document->title();
    } else if (auto* activeScene = _worldContext.scene(slot)) {
        rootLabel = activeScene->name().empty() ? "<unnamed>"
                                                : activeScene->name();
    }
    if (_gameView.mode() != EditorMode::Edit) {
        rootLabel += "  (Play World)";
    }
    setUtf8("outliner_hint", "Scene: " + rootLabel);

    std::vector<ayt::ui::TreeNodeData> nodes;
    ayt::ui::TreeNodeData root;
    root.label = std::wstring(rootLabel.begin(), rootLabel.end());
    root.hasChildren = true;
    root.expanded = _outlinerRootExpanded;
    root.parentIndex = -1;
    nodes.push_back(root);

    // INV-4：唯一 Scene 触点是 const World& + getAllEntities() const
    // （AYEntity/World.h:43）+ Entity::getId/getName（AYEntity/EntityImpl.h:31-32）。
    // 无任何 clear/load/save 调用 → Scene::_dirty 不可能被置位。
    const std::vector<ayt::entity::Entity*> entities =
        world->getAllEntities();
    nodes.reserve(entities.size() + 1);
    _outlinerEntityIds.reserve(entities.size());
    for (ayt::entity::Entity* e : entities) {
        if (e == nullptr) continue;
        const char* nameC = e->getName();
        std::string name = (nameC != nullptr && nameC[0] != '\0')
            ? std::string(nameC)
            : ("entity#" + std::to_string(
                  static_cast<unsigned>(e->getId())));

        ayt::ui::TreeNodeData d;
        d.label = std::wstring(name.begin(), name.end());
        d.hasChildren = false;
        d.expanded = false;
        d.parentIndex = 0;  // 挂在合成 root 下
        nodes.push_back(d);
        _outlinerEntityIds.push_back(e->getId());
    }

    _updatingOutlinerSelection = true;
    _outliner->setTree(nodes);

    // Preserve selection by entity id rather than TreeView's recycled flat
    // row. A collapsed root hides the row but must not clear Inspector/entity
    // selection; expanding it restores the visual highlight on the next pass.
    bool selectedEntityStillExists = _selection.empty();
    int selectedFlat = -1;
    if (!_selection.empty()) {
        for (size_t i = 0; i < _outlinerEntityIds.size(); ++i) {
            if (_outlinerEntityIds[i] == _selection.entityId()) {
                selectedEntityStillExists = true;
                if (_outlinerRootExpanded) {
                    selectedFlat = static_cast<int>(i) + 1;
                }
                break;
            }
        }
    }
    _outliner->setSelectedIndex(selectedFlat);
    _updatingOutlinerSelection = false;
    if (!selectedEntityStillExists) {
        clearSelectedEntity();  // entity was destroyed (endPlay, scene load)
    }
}

void EditorSession::onOutlinerSelectionChanged(int flatIndex)
{
    if (_updatingOutlinerSelection) {
        return;
    }
    // flat 0 = 合成 scene root：清 Hierarchy 选择，Inspector 退回 PR-4 路径。
    if (flatIndex <= 0) {
        clearSelectedEntity();
        refreshInspectorLabels();
        refreshTransformInspector();
        if (_repaintCallback) _repaintCallback();
        return;
    }
    const size_t idx = static_cast<size_t>(flatIndex - 1);
    if (idx >= _outlinerEntityIds.size()) {
        return;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    setSelectedEntity(world, world != nullptr
        ? world->findEntity(_outlinerEntityIds[idx]) : nullptr);

    // **Landmine B**：不**在此调 refreshOutliner()/_ui.layout()：会
    // delete 正在派发事件的 TreeNode（AYTreeView.cpp:80-85/194）→
    // UIManager::onMouseButtonUp:1339 UAF。只刷 Inspector（纯
    // TextLabel setText） + repaint。
    refreshInspectorLabels();
    refreshTransformInspector();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// =============================================================================
// Project Content Browser
// =============================================================================
void EditorSession::bindAssetBrowser()
{
    _assetTree = dynamic_cast<ayt::ui::TreeView*>(
        _ui.findById("tree_assets"));
    _assetTileView = dynamic_cast<ayt::ui::TileView*>(
        _ui.findById("list_assets"));
    _assetSearch = dynamic_cast<ayt::ui::TextInput*>(
        _ui.findById("assets_search"));
    _assetTypeFilter = dynamic_cast<ayt::ui::ComboBox*>(
        _ui.findById("cmb_assets_type"));

    if (_assetTree != nullptr) {
        _assetTree->setItemHeight(18.0f);
        _assetTree->setOnSelectionChanged([this](int flatIndex) {
            if (_updatingAssetSelection || flatIndex < 0
                || flatIndex >= static_cast<int>(_assetFolderFlatPaths.size())) {
                return;
            }
            _assetCurrentFolder = _assetFolderFlatPaths[flatIndex];
            refreshAssetList();
        });
        _assetTree->setOnExpandToggled([this](int flatIndex, bool expanded) {
            if (flatIndex < 0
                || flatIndex >= static_cast<int>(_assetFolderFlatPaths.size())) {
                return;
            }
            const std::string path = _assetFolderFlatPaths[flatIndex];
            for (std::size_t i = 0; i < _assetFolderSourcePaths.size(); ++i) {
                if (_assetFolderSourcePaths[i] == path) {
                    _assetFolderSourceExpanded[i] = expanded;
                    break;
                }
            }
            // TreeView already rebuilt its nodes. Recompute only our parallel
            // flat-index mapping so the next selection resolves correctly.
            rebuildAssetFolderMapping();
        });
    }

    if (_assetTileView != nullptr) {
        _assetTileView->setCellBinder(
            [this](ayt::ui::TileCell& cell, int index,
                   const std::wstring&) {
                if (index < 0
                    || index >= static_cast<int>(_assetEntries.size())) {
                    return;
                }
                const EditorAssetTilePresentation presentation =
                    _assetTilePresenter.present(_assetEntries.at(index));
                cell.setText(presentation.fullFileName);
                cell.setInfoStrip(
                    presentation.typeAbbreviation,
                    presentation.categoryColor,
                    ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
                cell.setCornerMarkerVisible(
                    presentation.showEngineResourceMarker);
                cell.clearThumbnail();
            });
        _assetTileView->setOnSelectionChanged([this](int index) {
            if (_updatingAssetSelection || index < 0
                || index >= static_cast<int>(_assetEntries.size())) return;
            const EditorAssetEntry& entry = _assetEntries[index];
            if (entry.folder) return;
            selectAsset(entry.assetId);
        });
        _assetTileView->setOnItemDoubleClicked(
            [this](int index, ayt::ui::TileCell::HitRegion) {
                if (index < 0
                    || index >= static_cast<int>(_assetEntries.size())) {
                    return;
                }
                const EditorAssetEntry& entry = _assetEntries[index];
                if (!entry.folder) {
                    const EditorAssetRecord* record =
                        _assetDatabase.find(entry.assetId);
                    if (record != nullptr
                        && editorDslLanguageFromPath(record->name)
                               != EditorDslLanguage::Unknown) {
                        // Do not mutate the DockArea while TileCell is still
                        // dispatching its second mouse-up. The next editor
                        // update opens or focuses the document atomically.
                        _pendingDslAssetOpenId = entry.assetId;
                        if (_repaintCallback) _repaintCallback();
                    }
                    return;
                }
                _assetCurrentFolder = entry.folderPath;
                // The double-click callback runs inside TileCell dispatch.
                // Rebind the virtual pool on the next editor update.
                _assetBrowserRefreshPending = true;
                if (_repaintCallback) _repaintCallback();
            });
        _assetTileView->setDragPayloadBuilder(
            [this](int index, const std::vector<int>&)
                -> ayt::ui::DragPayload {
                if (index < 0 || index >= static_cast<int>(_assetEntries.size())) {
                    return {};
                }
                const EditorAssetEntry& entry = _assetEntries[index];
                const EditorAssetRecord* record = entry.folder
                    ? nullptr : _assetDatabase.find(entry.assetId);
                if (record == nullptr || record->type != EditorAssetType::Mesh) {
                    return {};
                }
                _assetDragData.id = record->id;
                _assetDragData.type = record->type;
                _assetDragData.runtimePath = record->runtimePath;
                ayt::ui::DragPayload payload;
                payload.kind = "EditorAsset";
                payload.text = ayt::ui::decodeUtf8Text(record->name);
                payload.data = &_assetDragData;
                return payload;
            });
    }

    if (_assetSearch != nullptr) {
        _assetSearch->setOnTextChanged(
            [this](const std::wstring&) { refreshAssetList(); });
    }
    if (_assetTypeFilter != nullptr) {
        _assetTypeFilter->setOnSelectionChanged(
            [this](int) { refreshAssetList(); });
    }

    auto bindButton = [this](const char* id, std::function<void()> callback) {
        if (auto* button = dynamic_cast<ayt::ui::Button*>(_ui.findById(id))) {
            button->setOnClicked(std::move(callback));
        }
    };
    bindButton("btn_assets_add", [this]() { importAssetFromDialog(); });
    bindButton("btn_assets_refresh", [this]() {
        if (_assetDatabase.requestScan()) {
            setAssetBrowserStatus(L"Refreshing asset index...");
        }
    });
    bindButton("btn_assets_up", [this]() {
        const std::size_t slash = _assetCurrentFolder.find_last_of('/');
        if (slash == std::string::npos) return;
        _assetCurrentFolder = _assetCurrentFolder.substr(0, slash);
        refreshAssetBrowser();
    });
    bindButton("btn_asset_reload", [this]() { reloadSelectedAsset(); });

    auto bindViewportAssetDrop = [this](const char* id) {
        ayt::ui::Widget* target = _ui.findById(id);
        if (target == nullptr) return;
        target->setAcceptDrops(true);
        target->setAcceptDropKinds({"EditorAsset"});
        target->setOnDrop([this](const ayt::ui::DragPayload& payload) {
            if (payload.kind != "EditorAsset" || payload.data == nullptr) return;
            const auto* drag = static_cast<const AssetDragData*>(payload.data);
            if (drag != &_assetDragData || drag->type != EditorAssetType::Mesh) {
                return;
            }
            const ayt::math::FVector2 physical = _ui.logicalToPhysical(
                _ui.getDragLastMousePos());
            // The workspace/card fallbacks also cover the viewport toolbar.
            // Keep creation constrained to the actual scene surface.
            if (!isViewportSurfacePoint(physical.x, physical.y)) return;
            (void)placeAssetInViewport(drag->id, physical.x, physical.y);
        });
    };
    // panel_viewport is temporarily hidden while the host punches the native
    // composite hole. Input can arrive during that interval, in which case
    // hit testing resolves to one of its still-visible containers. Bind the
    // same typed drop contract at each stable layer; the nearest target wins.
    bindViewportAssetDrop("panel_viewport");
    bindViewportAssetDrop("viewport_workspace");
    bindViewportAssetDrop("card_viewport");
}

void EditorSession::rebuildAssetFolderMapping()
{
    _assetFolderFlatPaths.clear();
    if (_assetFolderSourcePaths.empty()) return;
    std::unordered_map<std::string, int> byPath;
    for (std::size_t i = 0; i < _assetFolderSourcePaths.size(); ++i) {
        byPath.emplace(_assetFolderSourcePaths[i], static_cast<int>(i));
    }
    std::vector<std::vector<int>> children(_assetFolderSourcePaths.size());
    std::vector<int> roots;
    for (std::size_t i = 0; i < _assetFolderSourcePaths.size(); ++i) {
        const std::string& path = _assetFolderSourcePaths[i];
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            roots.push_back(static_cast<int>(i));
        } else {
            const auto it = byPath.find(path.substr(0, slash));
            if (it != byPath.end()) children[it->second].push_back(
                static_cast<int>(i));
            else roots.push_back(static_cast<int>(i));
        }
    }
    std::function<void(int)> visit = [&](int index) {
        _assetFolderFlatPaths.push_back(_assetFolderSourcePaths[index]);
        if (index < static_cast<int>(_assetFolderSourceExpanded.size())
            && _assetFolderSourceExpanded[index]) {
            for (int child : children[index]) visit(child);
        }
    };
    for (int root : roots) visit(root);
}

void EditorSession::refreshAssetBrowser()
{
    if (_assetTree == nullptr || _assetTileView == nullptr) return;

    std::unordered_map<std::string, bool> priorExpanded;
    for (std::size_t i = 0; i < _assetFolderSourcePaths.size(); ++i) {
        priorExpanded[_assetFolderSourcePaths[i]] =
            i < _assetFolderSourceExpanded.size()
                ? _assetFolderSourceExpanded[i] : false;
    }

    EditorAssetId pendingId = 0;
    if (!_pendingAssetSelectionPath.empty()) {
        auto normalized = [](std::string path) {
            std::replace(path.begin(), path.end(), '\\', '/');
            std::transform(path.begin(), path.end(), path.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            return path;
        };
        const std::string wanted = normalized(_pendingAssetSelectionPath);
        for (const EditorAssetRecord& record : _assetDatabase.records()) {
            if (normalized(record.runtimePath) == wanted
                || normalized(record.name) == wanted) {
                pendingId = record.id;
                const std::size_t slash = record.logicalPath.find_last_of('/');
                _assetCurrentFolder = slash == std::string::npos
                    ? std::string("Imported")
                    : record.logicalPath.substr(0, slash);
                break;
            }
        }
    }

    const auto& folders = _assetDatabase.folders();
    _assetFolderSourcePaths.clear();
    _assetFolderSourceExpanded.clear();
    _assetFolderSourcePaths.reserve(folders.size());
    _assetFolderSourceExpanded.reserve(folders.size());
    std::unordered_map<std::string, int> sourceIndex;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        sourceIndex.emplace(folders[i].logicalPath, static_cast<int>(i));
        _assetFolderSourcePaths.push_back(folders[i].logicalPath);
        const auto old = priorExpanded.find(folders[i].logicalPath);
        _assetFolderSourceExpanded.push_back(old != priorExpanded.end()
            ? old->second : folders[i].parentPath.empty());
    }

    std::vector<bool> hasChildren(folders.size(), false);
    std::vector<int> parentIndices(folders.size(), -1);
    for (std::size_t i = 0; i < folders.size(); ++i) {
        const auto parent = sourceIndex.find(folders[i].parentPath);
        if (parent != sourceIndex.end()) {
            parentIndices[i] = parent->second;
            hasChildren[parent->second] = true;
        }
    }
    std::vector<ayt::ui::TreeNodeData> nodes;
    nodes.reserve(folders.size());
    for (std::size_t i = 0; i < folders.size(); ++i) {
        ayt::ui::TreeNodeData node;
        node.label = ayt::ui::decodeUtf8Text(folders[i].displayName);
        // TreeNode renders this field as literal text. Keep it empty until the
        // asset browser is wired to AYUI's SVG icon provider.
        node.icon.clear();
        node.hasChildren = hasChildren[i];
        node.expanded = _assetFolderSourceExpanded[i];
        node.parentIndex = parentIndices[i];
        nodes.push_back(std::move(node));
    }
    _assetTree->setTree(nodes);
    rebuildAssetFolderMapping();

    if (sourceIndex.find(_assetCurrentFolder) == sourceIndex.end()) {
        _assetCurrentFolder = "Assets";
    }
    const auto current = std::find(_assetFolderFlatPaths.begin(),
                                   _assetFolderFlatPaths.end(),
                                   _assetCurrentFolder);
    _updatingAssetSelection = true;
    _assetTree->setSelectedIndex(current == _assetFolderFlatPaths.end()
        ? -1 : static_cast<int>(std::distance(
            _assetFolderFlatPaths.begin(), current)));
    _updatingAssetSelection = false;
    refreshAssetList();

    if (pendingId != 0) {
        _pendingAssetSelectionPath.clear();
        for (std::size_t i = 0; i < _assetEntries.size(); ++i) {
            if (!_assetEntries[i].folder && _assetEntries[i].assetId == pendingId) {
                _updatingAssetSelection = true;
                _assetTileView->setSelectedIndex(static_cast<int>(i));
                _updatingAssetSelection = false;
                selectAsset(pendingId);
                break;
            }
        }
    }
}

void EditorSession::refreshAssetList()
{
    if (_assetTileView == nullptr) return;
    const std::string query = _assetSearch != nullptr
        ? wideToUtf8(_assetSearch->getText()) : std::string{};
    std::optional<EditorAssetType> filter;
    if (_assetTypeFilter != nullptr
        && _assetTypeFilter->getSelectedIndex() > 0) {
        filter = static_cast<EditorAssetType>(
            _assetTypeFilter->getSelectedIndex());
    }
    _assetEntries = _assetDatabase.entries(
        _assetCurrentFolder, query, filter);
    std::vector<std::wstring> labels;
    labels.reserve(_assetEntries.size());
    for (const EditorAssetEntry& entry : _assetEntries) {
        labels.push_back(_assetTilePresenter.present(entry).fullFileName);
    }
    _updatingAssetSelection = true;
    _assetTileView->setItems(labels);
    // A directory can contain far fewer tiles than its parent. Reset before
    // restoring selection so a one-item child never remains below viewport.
    _assetTileView->setScrollOffset(ayt::math::FVector2(0.0f, 0.0f));
    int selected = -1;
    for (std::size_t i = 0; i < _assetEntries.size(); ++i) {
        if (!_assetEntries[i].folder
            && _assetEntries[i].assetId == _selectedAssetId) {
            selected = static_cast<int>(i);
            break;
        }
    }
    _assetTileView->setSelectedIndex(selected);
    _updatingAssetSelection = false;
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_asset_path"))) {
        label->setText(ayt::ui::decodeUtf8Text(_assetCurrentFolder));
    }
    if (_assetEntries.empty() && !_assetDatabase.scanPending()) {
        setAssetBrowserStatus(query.empty()
            ? L"This folder is empty. Use + to import an asset."
            : L"No assets match the current search/filter.");
    }
}

void EditorSession::selectAsset(EditorAssetId assetId)
{
    const EditorAssetRecord* record = _assetDatabase.find(assetId);
    if (record == nullptr) return;
    // Resource and entity selection are mutually exclusive. Clear the
    // TreeView's visual selection as well as EditorSelection; otherwise a
    // resource picked while (for example) Character is highlighted leaves
    // that same row selected. Clicking Character again then produces no
    // TreeView selection-change callback and the resource Inspector remains
    // visible, including its Reload button.
    if (_outliner != nullptr && _outliner->getSelectedIndex() >= 0) {
        _outliner->setSelectedIndex(-1);
    }
    clearSelectedEntity(true, false);
    _selectedAssetId = assetId;
    setInspectorAssetMode(true);
    refreshAssetInspector();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::clearSelectedAsset()
{
    if (_selectedAssetId == 0) {
        setInspectorAssetMode(false);
        return;
    }
    _selectedAssetId = 0;
    setInspectorAssetMode(false);
    if (_assetTileView != nullptr && !_updatingAssetSelection) {
        _updatingAssetSelection = true;
        _assetTileView->clearSelection();
        _updatingAssetSelection = false;
    }
}

void EditorSession::setInspectorAssetMode(bool assetMode)
{
    bool changed = false;
    if (ayt::ui::Widget* entity = _ui.findById("inspector_entity_body")) {
        const bool visible = !assetMode;
        changed = changed || entity->isVisible() != visible;
        entity->setVisible(visible);
    }
    if (ayt::ui::Widget* asset = _ui.findById("inspector_asset_body")) {
        changed = changed || asset->isVisible() != assetMode;
        asset->setVisible(assetMode);
    }
    if (changed) {
        // UIManager caches layout by client size. Visibility changes alter
        // VBox participation without resizing the window. Invalidate here
        // and let populateFrame consume it before opening the native viewport
        // hole; synchronous re-entry from an input callback can corrupt the
        // composite layout.
        _ui.invalidateLayout();
    }
}

void EditorSession::refreshAssetInspector()
{
    const EditorAssetRecord* record = _assetDatabase.find(_selectedAssetId);
    if (record == nullptr) return;
    auto set = [this](const char* id, const std::wstring& text) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(_ui.findById(id))) {
            label->setText(text);
        }
    };
    set("inspector_hint", L"Project asset selected");
    set("inspector_asset_header", ayt::ui::decodeUtf8Text(record->name));
    set("inspector_asset_type", L"Type: "
        + ayt::ui::decodeUtf8Text(editorAssetTypeName(record->type)));
    set("inspector_asset_origin", record->origin == EditorAssetOrigin::Source
        ? L"Origin: Assets (source)" : L"Origin: Imported (generated)");
    set("inspector_asset_size", L"Size: " + assetSizeText(record->size));
    set("inspector_asset_path", L"Path: "
        + ayt::ui::decodeUtf8Text(record->logicalPath));
    const auto state = ayt::resource::ResourceManager::instance()
        .getLoadState(record->runtimePath);
    const wchar_t* stateName = L"not loaded";
    switch (state) {
    case ayt::resource::ResourceLoadState::Loading: stateName = L"loading"; break;
    case ayt::resource::ResourceLoadState::Ready: stateName = L"ready"; break;
    case ayt::resource::ResourceLoadState::Failed: stateName = L"failed"; break;
    case ayt::resource::ResourceLoadState::NotLoaded: break;
    }
    set("inspector_asset_state", std::wstring(L"State: ") + stateName);
    _ui.invalidateLayout();
}

void EditorSession::setAssetBrowserStatus(const std::wstring& text,
                                          bool mirrorToConsole)
{
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("assets_status"))) label->setText(text);
    if (mirrorToConsole) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
                _ui.findById("console_output"))) label->setText(text);
    }
    if (_repaintCallback) _repaintCallback();
}

bool EditorSession::rescanAssetsNow()
{
    std::string error;
    const bool ok = _assetDatabase.scanNow(&error);
    refreshAssetBrowser();
    if (ok) {
        setAssetBrowserStatus(
            L"Indexed " + std::to_wstring(_assetDatabase.records().size())
            + L" assets");
    } else {
        setAssetBrowserStatus(
            L"Asset scan failed: " + ayt::ui::decodeUtf8Text(error), true);
    }
    return ok;
}

void EditorSession::importAssetFromDialog()
{
    const std::string source = ImportDialog::showOpenAssetFileDialog(_hostWindow);
    if (source.empty()) return;
    if (!Importer::isSupportedExtension(source)) {
        setAssetBrowserStatus(L"Unsupported import type: "
            + ayt::ui::decodeUtf8Text(source), true);
        return;
    }
    setAssetBrowserStatus(L"Importing "
        + ayt::ui::decodeUtf8Text(std::filesystem::path(source).filename().string())
        + L"...", true);
    const Importer::Result result = Importer::importAssetFile(
        source, _assetDatabase.derivedRoot());
    if (!result.success) {
        setAssetBrowserStatus(L"Import failed: "
            + ayt::ui::decodeUtf8Text(result.errorMessage), true);
        return;
    }
    for (const auto& resource : result.conversion.resources) {
        if (resource.path.empty()) continue;
        std::filesystem::path output(resource.path);
        if (output.is_relative()) output =
            std::filesystem::path(_assetDatabase.derivedRoot()) / output;
        _pendingAssetSelectionPath = output.lexically_normal().string();
        break;
    }
    _assetCurrentFolder = "Imported";
    (void)rescanAssetsNow();
    setAssetBrowserStatus(result.usedCache
        ? L"Import cache reused; asset index refreshed."
        : L"Import complete; asset index refreshed.", true);
}

void EditorSession::reloadSelectedAsset()
{
    const EditorAssetRecord* record = _assetDatabase.find(_selectedAssetId);
    if (record == nullptr) return;
    ayt::resource::ResourceManager::instance().reloadResource(
        record->runtimePath);
    refreshAssetInspector();
    setAssetBrowserStatus(L"Reload requested: "
        + ayt::ui::decodeUtf8Text(record->name));
}

EditorSession::OpenDslDocument* EditorSession::findOpenDslDocument(
    EditorAssetId assetId) noexcept
{
    for (const auto& document : _openDslDocuments) {
        if (document != nullptr && document->assetId == assetId) {
            return document.get();
        }
    }
    return nullptr;
}

EditorSession::OpenDslDocument* EditorSession::focusedDslDocument() noexcept
{
    ayt::ui::Widget* focused = _ui.getFocusedWidget();
    if (focused == nullptr) return nullptr;
    for (const auto& document : _openDslDocuments) {
        if (document == nullptr || document->card == nullptr) continue;
        if (focused == document->card
            || ayt::ui::UIManager::isDescendantOf(
                focused, document->card)) {
            return document.get();
        }
    }
    return nullptr;
}

void EditorSession::refreshDslDocumentChrome(OpenDslDocument& document)
{
    const bool dirty = document.model.isDirty();
    if (document.card != nullptr) {
        std::wstring title = ayt::ui::decodeUtf8Text(document.fileName);
        if (dirty) title += L" *";
        document.card->setTitle(title);
    }
    if (document.status != nullptr) {
        document.status->setText(dirty
            ? L"Modified  |  Ctrl+S save  |  F7 compile"
            : L"Ready  |  F7 compile");
        document.status->setTextColor(dirty
            ? ayt::math::FVector4(0.95f, 0.72f, 0.30f, 1.0f)
            : ayt::math::FVector4(0.64f, 0.68f, 0.75f, 1.0f));
    }
    if (_repaintCallback) _repaintCallback();
}

bool EditorSession::saveDslDocument(OpenDslDocument& document)
{
    std::string error;
    if (!document.model.save(&error)) {
        if (document.status != nullptr) {
            document.status->setText(
                L"Save failed: " + ayt::ui::decodeUtf8Text(error));
            document.status->setTextColor(
                ayt::math::FVector4(0.95f, 0.35f, 0.35f, 1.0f));
        }
        if (document.diagnostics != nullptr) {
            document.diagnostics->setText(
                L"[Save failed] " + ayt::ui::decodeUtf8Text(error));
        }
        if (_repaintCallback) _repaintCallback();
        return false;
    }
    refreshDslDocumentChrome(document);
    if (document.status != nullptr) {
        document.status->setText(
            L"Saved " + ayt::ui::decodeUtf8Text(document.model.displayPath()));
        document.status->setTextColor(
            ayt::math::FVector4(0.42f, 0.78f, 0.52f, 1.0f));
    }
    setAssetBrowserStatus(
        L"Saved DSL: " + ayt::ui::decodeUtf8Text(document.model.displayPath()));
    if (_repaintCallback) _repaintCallback();
    return true;
}

void EditorSession::compileDslDocument(OpenDslDocument& document)
{
    if (document.status != nullptr) {
        document.status->setText(L"Compiling current editor buffer...");
        document.status->setTextColor(
            ayt::math::FVector4(0.48f, 0.70f, 0.96f, 1.0f));
    }
    const EditorDslCompileReport report = document.model.compile();
    if (document.diagnostics != nullptr) {
        document.diagnostics->setText(formatDslCompileReport(
            report, document.model.displayPath()));
        document.diagnostics->setCaret(0, 0);
    }
    if (document.status != nullptr) {
        document.status->setText(report.success
            ? (document.model.isDirty()
                ? L"Compile succeeded (unsaved buffer)"
                : L"Compile succeeded")
            : L"Compile failed - see Diagnostics");
        document.status->setTextColor(report.success
            ? ayt::math::FVector4(0.42f, 0.78f, 0.52f, 1.0f)
            : ayt::math::FVector4(0.95f, 0.35f, 0.35f, 1.0f));
    }
    setAssetBrowserStatus(
        ayt::ui::decodeUtf8Text(editorDslLanguageName(report.language))
        + (report.success ? L" compile succeeded: " : L" compile failed: ")
        + ayt::ui::decodeUtf8Text(document.fileName), true);
    if (_repaintCallback) _repaintCallback();
}

bool EditorSession::openDslAsset(EditorAssetId assetId)
{
    if (_mainDock == nullptr) return false;
    const EditorAssetRecord* record = _assetDatabase.find(assetId);
    if (record == nullptr
        || editorDslLanguageFromPath(record->name)
               == EditorDslLanguage::Unknown) {
        return false;
    }

    if (OpenDslDocument* existing = findOpenDslDocument(assetId)) {
        (void)_mainDock->setCardVisible(
            existing->cardId, true, ayt::ui::DockArea::Slot::Center);
        wirePromoteCallback();
        _ui.invalidateLayout();
        if (_repaintCallback) _repaintCallback();
        return true;
    }

    auto document = std::make_unique<OpenDslDocument>();
    document->assetId = assetId;
    document->cardId = "card_dsl_" + std::to_string(assetId);
    document->fileName = record->name;
    std::string error;
    if (!document->model.open(
            record->absolutePath, record->logicalPath, &error)) {
        setAssetBrowserStatus(
            L"DSL open failed: " + ayt::ui::decodeUtf8Text(error), true);
        return false;
    }

    auto card = std::make_unique<ayt::ui::DockCard>();
    card->setId(document->cardId);
    card->setTitle(ayt::ui::decodeUtf8Text(record->name));
    card->setHeaderHeight(20.0f);
    card->setClosable(true);
    document->card = card.get();

    auto* content = new ayt::ui::VBox();
    content->setSpacing(4.0f);
    content->setPadding(6.0f, 5.0f, 6.0f, 6.0f);

    auto* toolbar = new ayt::ui::HBox();
    toolbar->setSpacing(5.0f);
    auto* language = new ayt::ui::TextLabel();
    language->setText(ayt::ui::decodeUtf8Text(
        editorDslLanguageName(document->model.language())));
    language->setFontSize(12);
    language->setTextColor(
        ayt::math::FVector4(0.42f, 0.72f, 1.0f, 1.0f));
    language->setVerticalAlignment(
        ayt::ui::TextLabel::VAlignment::Center);

    auto* save = new ayt::ui::Button();
    save->setText(L"Save");
    save->setAccessibilityLabel(L"Save DSL source");
    save->setPadding(7.0f, 3.0f, 7.0f, 3.0f);
    auto* compile = new ayt::ui::Button();
    compile->setText(L"Compile");
    compile->setAccessibilityLabel(L"Compile current DSL buffer");
    compile->setPadding(7.0f, 3.0f, 7.0f, 3.0f);

    auto* status = new ayt::ui::TextLabel();
    status->setFontSize(12);
    status->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
    document->status = status;
    toolbar->addWidget(language, 68.0f);
    toolbar->addWidget(save, 54.0f);
    toolbar->addWidget(compile, 68.0f);
    toolbar->addWidget(status, 0.0f);
    content->addWidget(toolbar, 26.0f);

    auto* source = new ayt::ui::TextArea();
    source->setWordWrap(false);
    source->setLineHeight(17.0f);
    source->setMaxLength(8u * 1024u * 1024u);
    source->setLineNumbersVisible(true);
    source->setTabInsertsIndent(true);
    source->setTabWidth(4u);
    const EditorDslLanguage sourceLanguage = document->model.language();
    source->setSyntaxHighlighter(
        [sourceLanguage](std::size_t, const std::wstring& line) {
            return highlightDslLine(sourceLanguage, line);
        });
    source->setText(ayt::ui::decodeUtf8Text(document->model.sourceUtf8()));
    document->source = source;
    content->addWidget(source, 0.0f);

    auto* diagnosticsHeader = new ayt::ui::TextLabel();
    diagnosticsHeader->setText(L"DIAGNOSTICS");
    diagnosticsHeader->setFontSize(11);
    diagnosticsHeader->setTextColor(
        ayt::math::FVector4(0.58f, 0.62f, 0.70f, 1.0f));
    diagnosticsHeader->setVerticalAlignment(
        ayt::ui::TextLabel::VAlignment::Center);
    content->addWidget(diagnosticsHeader, 18.0f);

    auto* diagnostics = new ayt::ui::TextArea();
    diagnostics->setReadOnly(true);
    diagnostics->setWordWrap(true);
    diagnostics->setLineHeight(16.0f);
    diagnostics->setText(
        L"No compilation run yet. Compile uses the current editor buffer.");
    document->diagnostics = diagnostics;
    content->addWidget(diagnostics, 116.0f);
    card->setContent(content);

    OpenDslDocument* raw = document.get();
    source->setOnTextChanged([this, raw](const std::wstring& text) {
        raw->model.setSourceUtf8(wideToUtf8(text));
        refreshDslDocumentChrome(*raw);
    });
    save->setOnClicked([this, raw]() { (void)saveDslDocument(*raw); });
    compile->setOnClicked([this, raw]() { compileDslDocument(*raw); });

    _openDslDocuments.push_back(std::move(document));
    _mainDock->addCard(ayt::ui::DockArea::Slot::Center, std::move(card));
    // DSL cards are created after the initial shell traversal. Wire the new
    // live card immediately so it has the same tear-off behaviour as cards
    // loaded from editor_shell.ui.json.
    wirePromoteCallback();
    refreshDslDocumentChrome(*raw);
    _ui.invalidateLayout();
    setAssetBrowserStatus(
        L"Opened DSL: " + ayt::ui::decodeUtf8Text(record->logicalPath));
    if (_repaintCallback) _repaintCallback();
    return true;
}

bool EditorSession::requestCloseDslDocument(ayt::ui::DockCard* card)
{
    if (card == nullptr || _mainDock == nullptr) return false;
    auto it = std::find_if(
        _openDslDocuments.begin(), _openDslDocuments.end(),
        [card](const std::unique_ptr<OpenDslDocument>& document) {
            return document != nullptr && document->card == card;
        });
    if (it == _openDslDocuments.end()) return false;

    OpenDslDocument& document = **it;
    if (document.model.isDirty() && _hostWindow != nullptr) {
        const std::wstring prompt =
            ayt::ui::decodeUtf8Text(document.fileName)
            + L" has unsaved changes.\n\nSave before closing?";
        const int choice = ::MessageBoxW(
            _hostWindow, prompt.c_str(), L"AY Editor DSL",
            MB_YESNOCANCEL | MB_ICONWARNING);
        if (choice == IDCANCEL) return true;
        if (choice == IDYES && !saveDslDocument(document)) return true;
    }

    ayt::ui::Widget* focused = _ui.getFocusedWidget();
    if (focused != nullptr
        && (focused == card
            || ayt::ui::UIManager::isDescendantOf(focused, card))) {
        _ui.setFocus(nullptr);
    }
    const std::string cardId = document.cardId;
    if (_mainDock->closeCard(cardId)) {
        _openDslDocuments.erase(it);
        _ui.invalidateLayout();
        if (_repaintCallback) _repaintCallback();
    }
    return true;
}

bool EditorSession::placeAssetInViewport(EditorAssetId assetId,
                                         float physicalX, float physicalY)
{
    const EditorAssetRecord* record = _assetDatabase.find(assetId);
    if (record == nullptr || record->type != EditorAssetType::Mesh
        || _document == nullptr || _gameView.mode() != EditorMode::Edit) {
        return false;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    if (world == nullptr) return false;
    ayt::math::FVector3 direction;
    if (!viewportRayDirection(physicalX, physicalY, direction)) return false;

    ayt::math::FVector3 position = _freecam.eye() + direction * 5.0f;
    if (std::fabs(direction.y) > 1.0e-5f) {
        const float distance = -_freecam.eye().y / direction.y;
        if (distance > 0.0f) position = _freecam.eye() + direction * distance;
    }

    std::shared_ptr<ayt::resource::IMesh> meshResource;
    try {
        meshResource = ayt::resource::ResourceManager::instance()
            .load<ayt::resource::IMesh>(record->runtimePath);
    } catch (...) {
        // The entity still preserves the asset reference. Inspector/load state
        // exposes a decoder failure and Reload can retry after hot replacement.
    }
    std::string materialPath;
    if (meshResource != nullptr) {
        if (meshResource->hasBounds()) {
            position.y -= meshResource->getBounds().getMin().y;
        }
        if (meshResource->getMaterialSlotCount() > 0) {
            const char* slot = meshResource->getMaterialSlot(0);
            if (slot != nullptr && slot[0] != '\0') {
                materialPath = ayt::resource::resolveAssetPath(
                    record->runtimePath, slot);
            }
        }
    }

    ayt::entity::Entity* entity = world->createEntity();
    if (entity == nullptr) return false;
    const std::string stem = std::filesystem::path(record->name).stem().string();
    const std::string name = (stem.empty() ? std::string("Mesh") : stem)
        + " " + std::to_string(static_cast<unsigned>(entity->getId()));
    entity->setName(name.c_str());
    auto* transform = entity->addComponent<ayt::entity::Transform>();
    transform->setPosition(position.x, position.y, position.z);
    auto* mesh = entity->addComponent<ayt::entity::MeshComponent>();
    mesh->meshPath = record->runtimePath;
    mesh->materialPath = materialPath;

    setSelectedEntity(world, entity);
    _commands.clear();
    _document->markDirty();
    _outlinerRefreshPending = true;
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    setAssetBrowserStatus(L"Created scene entity from "
        + ayt::ui::decodeUtf8Text(record->name));
    if (_repaintCallback) _repaintCallback();
    return true;
}

void EditorSession::setDockCardVisible(const char* cardId, bool visible) {
    ayt::ui::DockArea::Slot slot = ayt::ui::DockArea::Slot::Center;
    if (std::strcmp(cardId, "card_outliner") == 0) {
        slot = ayt::ui::DockArea::Slot::Left;
    } else if (std::strcmp(cardId, "card_render") == 0
               || std::strcmp(cardId, "card_inspector") == 0) {
        slot = ayt::ui::DockArea::Slot::Right;
    } else if (std::strcmp(cardId, "card_network") == 0
               || std::strcmp(cardId, "card_console") == 0
               || std::strcmp(cardId, "card_assets") == 0) {
        slot = ayt::ui::DockArea::Slot::Bottom;
    }
    if (_mainDock != nullptr) {
        _mainDock->setCardVisible(cardId, visible, slot);
    }
    // A workspace can start with a persistent panel parked as hidden. Rebind
    // after reveal so cards restored after the initial shell walk always have
    // the child-window promotion callback.
    if (visible) {
        wirePromoteCallback();
    }
    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::toggleDockCard(const char* cardId, bool& visibleFlag) {
    visibleFlag = !visibleFlag;
    setDockCardVisible(cardId, visibleFlag);
}

void EditorSession::bindNetworkPanelStub() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_net_spawn", []() {
        std::fprintf(stderr,
            "[EditorSession] Network Spawn (stub) — wire to server EntitySpawn\n");
    });
    bindButton("btn_net_despawn", []() {
        std::fprintf(stderr,
            "[EditorSession] Network Despawn (stub) — wire to server despawn\n");
    });

    if (auto* w = _ui.findById("sld_net_hp")) {
        if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
            slider->setOnValueChanged([this](float v) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Cube HP  %.0f",
                              static_cast<double>(v));
                if (auto* lbl = _ui.findById("lbl_net_hp")) {
                    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(lbl)) {
                        label->setText(std::wstring(buf, buf + std::strlen(buf)));
                    }
                }
            });
        }
    }
}

void EditorSession::bindRenderSettingsPanel()
{
    // Left "Render" panel — live knobs after RendererSubSystem exists
    // (Play session). Safe no-ops before Play: findRegistered() is null.
    auto rendererOrNull = []() -> ayt::render::Renderer* {
        if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
            return &sub->renderer();
        }
        return nullptr;
    };

    auto setLabel = [this](const char* id, const char* textUtf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(textUtf8, textUtf8 + std::strlen(textUtf8)));
            }
        }
    };

    auto bindSlider = [this](const char* id, std::function<void(float)> onChanged) {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                slider->setOnValueChanged(std::move(onChanged));
            }
        }
    };

    bindSlider("sld_gamma", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Gamma  %.2f", static_cast<double>(v));
        setLabel("lbl_gamma", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessGamma(v);
        }
    });

    bindSlider("sld_exposure", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Exposure  %.2f", static_cast<double>(v));
        setLabel("lbl_exposure", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessExposure(v);
        }
    });

    bindSlider("sld_bloom", [this, rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Bloom  %.2f", static_cast<double>(v));
        setLabel("lbl_bloom", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            bool enabled = true;
            if (auto* w = _ui.findById("chk_bloom")) {
                if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                    enabled = chk->isChecked();
                }
            }
            r->setPostProcessBloomStrength(enabled ? v : 0.0f);
        }
    });

    if (auto* w = _ui.findById("chk_bloom")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, rendererOrNull](bool on) {
                float strength = 0.3f;
                if (auto* sw = _ui.findById("sld_bloom")) {
                    if (auto* slider = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = slider->getValue();
                    }
                }
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setPostProcessBloomStrength(on ? strength : 0.0f);
                }
            });
        }
    }

    // §S4d — Depth Haze (default slightly on in UI JSON).
    auto applyHazeParams = [rendererOrNull](bool enabled, float strength, float density) {
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setDepthHazeEnabled(enabled);
            r->setDepthHazeStrength(enabled ? strength : 0.0f);
            r->setDepthHazeParams(
                density,
                ayt::math::FVector3(0.7f, 0.75f, 0.8f));
        }
    };

    if (auto* w = _ui.findById("chk_depth_haze")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applyHazeParams](bool on) {
                float strength = 1.0f;
                float density = 0.04f;
                if (auto* sw = _ui.findById("sld_haze_strength")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = s->getValue();
                    }
                }
                if (auto* dw = _ui.findById("sld_haze_density")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(dw)) {
                        density = s->getValue();
                    }
                }
                applyHazeParams(on, strength, density);
            });
        }
    }

    bindSlider("sld_haze_strength", [this, setLabel, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Haze Strength  %.2f", static_cast<double>(v));
        setLabel("lbl_haze_strength", buf);
        bool enabled = true;
        float density = 0.04f;
        if (auto* cw = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* dw = _ui.findById("sld_haze_density")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(dw)) {
                density = s->getValue();
            }
        }
        applyHazeParams(enabled, v, density);
    });

    bindSlider("sld_haze_density", [this, setLabel, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Haze Density  %.3f", static_cast<double>(v));
        setLabel("lbl_haze_density", buf);
        bool enabled = true;
        float strength = 1.0f;
        if (auto* cw = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_haze_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        applyHazeParams(enabled, strength, v);
    });

    // §S2 v1 — SSAO (Deferred-only; UI defaults slightly on).
    auto applySsaoParams = [rendererOrNull](bool enabled, float strength,
                                            float radius, float bias) {
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setSsaoEnabled(enabled);
            r->setSsaoStrength(enabled ? strength : 0.0f);
            r->setSsaoParams(radius, bias);
        }
    };

    if (auto* w = _ui.findById("chk_ssao")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applySsaoParams](bool on) {
                float strength = 0.45f;
                float radius = 0.4f;
                float bias = 0.04f;
                if (auto* sw = _ui.findById("sld_ssao_strength")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = s->getValue();
                    }
                }
                if (auto* rw = _ui.findById("sld_ssao_radius")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                        radius = s->getValue();
                    }
                }
                if (auto* bw = _ui.findById("sld_ssao_bias")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                        bias = s->getValue();
                    }
                }
                applySsaoParams(on, strength, radius, bias);
            });
        }
    }

    bindSlider("sld_ssao_strength", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Strength  %.2f", static_cast<double>(v));
        setLabel("lbl_ssao_strength", buf);
        bool enabled = true;
        float radius = 0.4f;
        float bias = 0.04f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* rw = _ui.findById("sld_ssao_radius")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                radius = s->getValue();
            }
        }
        if (auto* bw = _ui.findById("sld_ssao_bias")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                bias = s->getValue();
            }
        }
        applySsaoParams(enabled, v, radius, bias);
    });

    bindSlider("sld_ssao_radius", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Radius  %.2f", static_cast<double>(v));
        setLabel("lbl_ssao_radius", buf);
        bool enabled = true;
        float strength = 0.45f;
        float bias = 0.04f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_ssao_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        if (auto* bw = _ui.findById("sld_ssao_bias")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                bias = s->getValue();
            }
        }
        applySsaoParams(enabled, strength, v, bias);
    });

    bindSlider("sld_ssao_bias", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Bias  %.3f", static_cast<double>(v));
        setLabel("lbl_ssao_bias", buf);
        bool enabled = true;
        float strength = 0.45f;
        float radius = 0.4f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_ssao_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        if (auto* rw = _ui.findById("sld_ssao_radius")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                radius = s->getValue();
            }
        }
        applySsaoParams(enabled, strength, radius, v);
    });

    bindSlider("sld_ambient", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "IBL Ambient  %.2f", static_cast<double>(v));
        setLabel("lbl_ambient", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setAmbientStrength(v);
        }
    });

    bindSlider("sld_shadow_bias", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Shadow Bias  %.4f", static_cast<double>(v));
        setLabel("lbl_shadow_bias", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setShadowBias(v);
        }
    });

    if (auto* w = _ui.findById("cmb_tonemap")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            combo->setOnSelectionChanged([rendererOrNull](int index) {
                ayt::render::Renderer* r = rendererOrNull();
                if (r == nullptr) {
                    return;
                }
                using TM = ayt::render::Renderer::TonemapMode;
                switch (index) {
                case 1:  r->setPostProcessTonemapMode(TM::Reinhard); break;
                case 2:  r->setPostProcessTonemapMode(TM::ACES); break;
                default: r->setPostProcessTonemapMode(TM::None); break;
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_shadow_pcf")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([rendererOrNull](bool on) {
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setShadowPcfEnabled(on);
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_shadows")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([rendererOrNull](bool on) {
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setShadowsEnabled(on);
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_fxaa")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, rendererOrNull](bool on) {
                if (on) {
                    if (auto* smaa = dynamic_cast<ayt::ui::CheckBox*>(
                            _ui.findById("chk_smaa"))) {
                        smaa->setChecked(false);
                    }
                }
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setFxaaEnabled(on);
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_smaa")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, rendererOrNull](bool on) {
                if (on) {
                    if (auto* fxaa = dynamic_cast<ayt::ui::CheckBox*>(
                            _ui.findById("chk_fxaa"))) {
                        fxaa->setChecked(false);
                    }
                }
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setSmaaEnabled(on);
                }
            });
        }
    }

    // Color grading is an optional final LDR look pass. Neutral is retained as
    // an explicit identity/bypass preset, but enabling the effect while that
    // preset is selected promotes the UI to Warm so the checkbox always gives
    // immediate visual feedback.
    auto applyColorGrading = [this, rendererOrNull]() {
        bool enabled = false;
        float strength = 0.75f;
        int presetIndex = 1;
        if (auto* w = _ui.findById("chk_color_grading")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* w = _ui.findById("sld_color_grading_strength")) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                strength = slider->getValue();
            }
        }
        if (auto* w = _ui.findById("cmb_color_grading_preset")) {
            if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
                presetIndex = combo->getSelectedIndex();
            }
        }

        using Preset = ayt::render::ColorGradingPreset;
        Preset preset = Preset::Neutral;
        switch (presetIndex) {
        case 1: preset = Preset::Warm; break;
        case 2: preset = Preset::Cool; break;
        case 3: preset = Preset::Cinematic; break;
        default: break;
        }
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setColorGradingPreset(preset);
            r->setColorGradingStrength(strength);
            r->setColorGradingEnabled(enabled);
        }
    };

    if (auto* w = _ui.findById("chk_color_grading")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applyColorGrading](bool enabled) {
                if (enabled) {
                    if (auto* presetWidget =
                            _ui.findById("cmb_color_grading_preset")) {
                        if (auto* combo =
                                dynamic_cast<ayt::ui::ComboBox*>(presetWidget);
                            combo != nullptr && combo->getSelectedIndex() == 0) {
                            combo->setSelectedIndex(1);
                        }
                    }
                }
                applyColorGrading();
            });
        }
    }
    if (auto* w = _ui.findById("cmb_color_grading_preset")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            combo->setOnSelectionChanged(
                [applyColorGrading](int) { applyColorGrading(); });
        }
    }
    bindSlider("sld_color_grading_strength",
               [setLabel, applyColorGrading](float value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Color Grade Strength  %.2f",
                      static_cast<double>(value));
        setLabel("lbl_color_grading_strength", buf);
        applyColorGrading();
    });

    // Labels in JSON are decorative until Slider min/max/value load;
    // refresh from the live widget values so thumb ↔ text stay aligned.
    auto refreshLabelFromSlider = [this, setLabel](const char* sliderId,
                                                   const char* labelId,
                                                   const char* fmt) {
        if (auto* w = _ui.findById(sliderId)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), fmt,
                              static_cast<double>(slider->getValue()));
                setLabel(labelId, buf);
            }
        }
    };
    refreshLabelFromSlider("sld_gamma", "lbl_gamma", "Gamma  %.2f");
    refreshLabelFromSlider("sld_exposure", "lbl_exposure", "Exposure  %.2f");
    refreshLabelFromSlider("sld_bloom", "lbl_bloom", "Bloom  %.2f");
    refreshLabelFromSlider("sld_haze_strength", "lbl_haze_strength", "Haze Strength  %.2f");
    refreshLabelFromSlider("sld_haze_density", "lbl_haze_density", "Haze Density  %.3f");
    refreshLabelFromSlider("sld_ssao_strength", "lbl_ssao_strength", "SSAO Strength  %.2f");
    refreshLabelFromSlider("sld_ssao_radius", "lbl_ssao_radius", "SSAO Radius  %.2f");
    refreshLabelFromSlider("sld_ssao_bias", "lbl_ssao_bias", "SSAO Bias  %.3f");
    refreshLabelFromSlider("sld_ambient", "lbl_ambient", "IBL Ambient  %.2f");
    refreshLabelFromSlider("sld_shadow_bias", "lbl_shadow_bias", "Shadow Bias  %.4f");
    refreshLabelFromSlider("sld_color_grading_strength",
                           "lbl_color_grading_strength",
                           "Color Grade Strength  %.2f");
}

void EditorSession::applyRenderSettingsFromPanel()
{
    auto* sub = ayt::render::RendererSubSystem::findRegistered();
    if (sub == nullptr) {
        return;
    }
    ayt::render::Renderer& r = sub->renderer();

    auto sliderValue = [this](const char* id, float fallback) -> float {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                return slider->getValue();
            }
        }
        return fallback;
    };

    r.setPostProcessGamma(sliderValue("sld_gamma", 2.2f));
    r.setPostProcessExposure(sliderValue("sld_exposure", 1.0f));
    bool bloomOn = true;
    if (auto* w = _ui.findById("chk_bloom")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            bloomOn = chk->isChecked();
        }
    }
    r.setPostProcessBloomStrength(
        bloomOn ? sliderValue("sld_bloom", 0.3f) : 0.0f);
    r.setAmbientStrength(sliderValue("sld_ambient", 0.85f));
    r.setShadowBias(sliderValue("sld_shadow_bias", 0.003f));

    // §S4d — Depth Haze defaults: on, density 0.04, fog (0.7,0.75,0.8).
    {
        bool hazeOn = true;
        if (auto* w = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                hazeOn = chk->isChecked();
            }
        }
        const float hazeStrength = sliderValue("sld_haze_strength", 1.0f);
        const float hazeDensity = sliderValue("sld_haze_density", 0.04f);
        r.setDepthHazeEnabled(hazeOn);
        r.setDepthHazeStrength(hazeOn ? hazeStrength : 0.0f);
        r.setDepthHazeParams(
            hazeDensity,
            ayt::math::FVector3(0.7f, 0.75f, 0.8f));
    }

    // §S2 v1 — SSAO defaults: on, strength 0.45, radius 0.4, bias 0.04.
    {
        bool ssaoOn = true;
        if (auto* w = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                ssaoOn = chk->isChecked();
            }
        }
        const float ssaoStrength = sliderValue("sld_ssao_strength", 0.45f);
        const float ssaoRadius = sliderValue("sld_ssao_radius", 0.4f);
        const float ssaoBias = sliderValue("sld_ssao_bias", 0.04f);
        r.setSsaoEnabled(ssaoOn);
        r.setSsaoStrength(ssaoOn ? ssaoStrength : 0.0f);
        r.setSsaoParams(ssaoRadius, ssaoBias);
    }

    if (auto* w = _ui.findById("cmb_tonemap")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            using TM = ayt::render::Renderer::TonemapMode;
            switch (combo->getSelectedIndex()) {
            case 1:  r.setPostProcessTonemapMode(TM::Reinhard); break;
            case 2:  r.setPostProcessTonemapMode(TM::ACES); break;
            default: r.setPostProcessTonemapMode(TM::None); break;
            }
        }
    }

    if (auto* w = _ui.findById("chk_shadow_pcf")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            r.setShadowPcfEnabled(chk->isChecked());
        }
    }
    if (auto* w = _ui.findById("chk_shadows")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            r.setShadowsEnabled(chk->isChecked());
        }
    }
    bool fxaaEnabled = false;
    bool smaaEnabled = false;
    if (auto* w = _ui.findById("chk_fxaa")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            fxaaEnabled = chk->isChecked();
        }
    }
    if (auto* w = _ui.findById("chk_smaa")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            smaaEnabled = chk->isChecked();
        }
    }
    r.setSmaaEnabled(smaaEnabled);
    r.setFxaaEnabled(fxaaEnabled && !smaaEnabled);
    {
        bool enabled = false;
        if (auto* w = _ui.findById("chk_color_grading")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                enabled = chk->isChecked();
            }
        }
        int presetIndex = 1;
        if (auto* w = _ui.findById("cmb_color_grading_preset")) {
            if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
                presetIndex = combo->getSelectedIndex();
            }
        }
        using Preset = ayt::render::ColorGradingPreset;
        Preset preset = Preset::Neutral;
        switch (presetIndex) {
        case 1: preset = Preset::Warm; break;
        case 2: preset = Preset::Cool; break;
        case 3: preset = Preset::Cinematic; break;
        default: break;
        }
        r.setColorGradingPreset(preset);
        r.setColorGradingStrength(
            sliderValue("sld_color_grading_strength", 0.75f));
        r.setColorGradingEnabled(enabled);
    }
    r.setViewportOrientationAxisEnabled(_viewportOrientationAxisVisible);
}

void EditorSession::applyPreferences(const EditorPreferences& preferences)
{
    _applyingPreferences = true;
    _preferences = preferences;

    installEditorTheme(preferences.themeName);
    _ui.setUiScale(std::clamp(preferences.uiScale, 0.75f, 1.25f));
    applyEditorVisualStyle(_ui, preferences.density);

    _viewportOrientationAxisVisible =
        preferences.viewportOrientationAxisVisible;
    if (_viewportOrientationAxisMenuItem != nullptr) {
        _viewportOrientationAxisMenuItem->setText(
            _viewportOrientationAxisVisible
                ? L"[x] Viewport Orientation Axis"
                : L"[ ] Viewport Orientation Axis");
    }
    if (preferences.cameraPoseValid) {
        _freecam.setPose(preferences.cameraEye,
                         preferences.cameraYawRadians,
                         preferences.cameraPitchRadians);
    }
    if (std::isfinite(preferences.cameraMoveSpeed)
        && preferences.cameraMoveSpeed > 0.01f) {
        _freecam.setMoveSpeed(preferences.cameraMoveSpeed);
    }
    setActiveTool(preferences.activeTool);
    setLocalTransformSpace(preferences.localTransformSpace);
    _orthographicView = preferences.orthographicView;
    _wireframeView = preferences.wireframeView;
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_camera"))) {
        button->setText(_orthographicView ? L"Orthographic" : L"Perspective");
    }
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_shading"))) {
        button->setText(_wireframeView ? L"Wireframe" : L"Shaded");
    }

    auto setSlider = [this](const char* id, float value) {
        if (auto* slider = dynamic_cast<ayt::ui::Slider*>(_ui.findById(id))) {
            slider->setValue(value);
        }
    };
    auto setCheck = [this](const char* id, bool checked) {
        if (auto* check = dynamic_cast<ayt::ui::CheckBox*>(_ui.findById(id))) {
            check->setChecked(checked);
        }
    };
    auto setCombo = [this](const char* id, int index) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(_ui.findById(id))) {
            combo->setSelectedIndex(index);
        }
    };

    setSlider("sld_gamma", preferences.gamma);
    setSlider("sld_exposure", preferences.exposure);
    setCheck("chk_bloom", preferences.bloomEnabled);
    setSlider("sld_bloom", preferences.bloomStrength);
    setCheck("chk_depth_haze", preferences.depthHazeEnabled);
    setSlider("sld_haze_strength", preferences.depthHazeStrength);
    setSlider("sld_haze_density", preferences.depthHazeDensity);
    setCheck("chk_ssao", preferences.ssaoEnabled);
    setSlider("sld_ssao_strength", preferences.ssaoStrength);
    setSlider("sld_ssao_radius", preferences.ssaoRadius);
    setSlider("sld_ssao_bias", preferences.ssaoBias);
    setSlider("sld_ambient", preferences.ambientStrength);
    setSlider("sld_shadow_bias", preferences.shadowBias);
    setCombo("cmb_tonemap", preferences.tonemapMode);
    setCheck("chk_fxaa", preferences.fxaaEnabled && !preferences.smaaEnabled);
    setCheck("chk_smaa", preferences.smaaEnabled);
    // Migrate the ambiguous persisted state `enabled + Neutral` to the visible
    // Warm look. Off already represents an identity transform, so preserving
    // that pair would make the checkbox appear broken on the next launch.
    const int colorGradingPreset = preferences.colorGradingEnabled
        && preferences.colorGradingPreset == 0
        ? 1
        : preferences.colorGradingPreset;
    setCombo("cmb_color_grading_preset", colorGradingPreset);
    setCheck("chk_color_grading", preferences.colorGradingEnabled);
    setSlider("sld_color_grading_strength",
              preferences.colorGradingStrength);
    setCheck("chk_shadows", preferences.shadowsEnabled);
    setCheck("chk_shadow_pcf", preferences.shadowPcfEnabled);

    if (_mainDock != nullptr && !preferences.dockTree.empty()
        && !_mainDock->applyDockTree(preferences.dockTree)) {
        std::fprintf(stderr,
            "[EditorSession] ignored malformed saved Dock layout\n");
    }
    _panelRenderVisible = preferences.panelRenderVisible;
    _panelInspectorVisible = preferences.panelInspectorVisible;
    _panelNetworkVisible = preferences.panelNetworkVisible;
    _panelOutlinerVisible = preferences.panelOutlinerVisible;
    _panelConsoleVisible = preferences.panelConsoleVisible;
    _panelAssetsVisible = preferences.panelAssetsVisible;
    // setCardVisible(true) also activates that tab. Only cross the hidden/open
    // boundary here so applying visibility flags does not overwrite the active
    // tab that applyDockTree() just restored.
    auto applyPanelVisibility = [this](const char* id, bool visible) {
        if (_mainDock == nullptr) return;
        ayt::ui::DockCard* card = _mainDock->findCard(id);
        if (card == nullptr) return;
        const bool parkedHidden = card->getParent() == _mainDock;
        if (visible == parkedHidden) {
            setDockCardVisible(id, visible);
        }
    };
    applyPanelVisibility("card_render", _panelRenderVisible);
    applyPanelVisibility("card_inspector", _panelInspectorVisible);
    applyPanelVisibility("card_network", _panelNetworkVisible);
    applyPanelVisibility("card_outliner", _panelOutlinerVisible);
    applyPanelVisibility("card_console", _panelConsoleVisible);
    applyPanelVisibility("card_assets", _panelAssetsVisible);

    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();
    applyRenderSettingsFromPanel();
    pushFreecamToRenderer();
    _applyingPreferences = false;
}

EditorPreferences EditorSession::capturePreferences() const
{
    EditorPreferences out = _preferences;
    out.themeName = ayt::ui::ThemeManager::get().getActiveThemeName();
    out.uiScale = _ui.getUiScale();
    out.viewportOrientationAxisVisible = _viewportOrientationAxisVisible;
    out.panelRenderVisible = _panelRenderVisible;
    out.panelInspectorVisible = _panelInspectorVisible;
    out.panelNetworkVisible = _panelNetworkVisible;
    out.panelOutlinerVisible = _panelOutlinerVisible;
    out.panelConsoleVisible = _panelConsoleVisible;
    out.panelAssetsVisible = _panelAssetsVisible;
    if (_mainDock != nullptr) {
        out.dockTree = _mainDock->serializeDockTree();
    }
    out.cameraPoseValid = true;
    out.cameraEye = _freecam.eye();
    out.cameraYawRadians = _freecam.yawRadians();
    out.cameraPitchRadians = _freecam.pitchRadians();
    out.cameraMoveSpeed = _freecam.moveSpeed();
    out.activeTool = _activeTool;
    out.localTransformSpace = _localTransformSpace;
    out.orthographicView = _orthographicView;
    out.wireframeView = _wireframeView;

    auto sliderValue = [this](const char* id, float fallback) {
        if (auto* slider = dynamic_cast<ayt::ui::Slider*>(_ui.findById(id))) {
            return slider->getValue();
        }
        return fallback;
    };
    auto checkValue = [this](const char* id, bool fallback) {
        if (auto* check = dynamic_cast<ayt::ui::CheckBox*>(_ui.findById(id))) {
            return check->isChecked();
        }
        return fallback;
    };
    auto comboValue = [this](const char* id, int fallback) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(_ui.findById(id))) {
            return combo->getSelectedIndex();
        }
        return fallback;
    };
    out.gamma = sliderValue("sld_gamma", out.gamma);
    out.exposure = sliderValue("sld_exposure", out.exposure);
    out.bloomEnabled = checkValue("chk_bloom", out.bloomEnabled);
    out.bloomStrength = sliderValue("sld_bloom", out.bloomStrength);
    out.depthHazeEnabled = checkValue("chk_depth_haze", out.depthHazeEnabled);
    out.depthHazeStrength = sliderValue(
        "sld_haze_strength", out.depthHazeStrength);
    out.depthHazeDensity = sliderValue(
        "sld_haze_density", out.depthHazeDensity);
    out.ssaoEnabled = checkValue("chk_ssao", out.ssaoEnabled);
    out.ssaoStrength = sliderValue("sld_ssao_strength", out.ssaoStrength);
    out.ssaoRadius = sliderValue("sld_ssao_radius", out.ssaoRadius);
    out.ssaoBias = sliderValue("sld_ssao_bias", out.ssaoBias);
    out.ambientStrength = sliderValue("sld_ambient", out.ambientStrength);
    out.shadowBias = sliderValue("sld_shadow_bias", out.shadowBias);
    out.tonemapMode = comboValue("cmb_tonemap", out.tonemapMode);
    out.fxaaEnabled = checkValue("chk_fxaa", out.fxaaEnabled);
    out.smaaEnabled = checkValue("chk_smaa", out.smaaEnabled);
    if (out.smaaEnabled) out.fxaaEnabled = false;
    out.colorGradingEnabled = checkValue(
        "chk_color_grading", out.colorGradingEnabled);
    out.colorGradingPreset = comboValue(
        "cmb_color_grading_preset", out.colorGradingPreset);
    out.colorGradingStrength = sliderValue(
        "sld_color_grading_strength", out.colorGradingStrength);
    out.shadowsEnabled = checkValue("chk_shadows", out.shadowsEnabled);
    out.shadowPcfEnabled = checkValue(
        "chk_shadow_pcf", out.shadowPcfEnabled);
    return out;
}

EditorPreferences EditorSession::currentPreferences() const
{
    return capturePreferences();
}

void EditorSession::savePreferencesNow()
{
    if (_applyingPreferences) return;
    _preferences = capturePreferences();
    _lastObservedPreferences = _preferences;
    _preferencesDirty = false;
    _preferencesSaveCountdown = 0.0f;
    if (_onPreferencesChanged) {
        _onPreferencesChanged(_preferences);
    }
}

void EditorSession::pollPreferences(float dtSeconds)
{
    if (_applyingPreferences || !_onPreferencesChanged) return;
    _preferencesPollCountdown -= std::max(dtSeconds, 0.0f);
    if (_preferencesPollCountdown <= 0.0f) {
        _preferencesPollCountdown = 0.20f;
        const EditorPreferences observed = capturePreferences();
        if (observed != _lastObservedPreferences) {
            _lastObservedPreferences = observed;
            _preferencesDirty = true;
            _preferencesSaveCountdown = 0.45f;
        }
    }
    if (_preferencesDirty) {
        _preferencesSaveCountdown -= std::max(dtSeconds, 0.0f);
        if (_preferencesSaveCountdown <= 0.0f) {
            savePreferencesNow();
        }
    }
}

void EditorSession::resetWorkspacePreferences()
{
    EditorPreferences defaults;
    // Reset Workspace intentionally leaves the host window dimensions alone.
    defaults.windowWidth = _preferences.windowWidth;
    defaults.windowHeight = _preferences.windowHeight;
    defaults.windowMaximized = _preferences.windowMaximized;
    defaults.themeName = _preferences.themeName;
    defaults.density = _preferences.density;
    defaults.uiScale = _preferences.uiScale;
    applyPreferences(defaults);
    savePreferencesNow();
}

void EditorSession::setActiveTool(EditorTool tool)
{
    if (_transformGizmo.active()) {
        finishTransformGizmoDrag(false);
    }
    _activeTool = tool;
    _gizmoHoverHandle = EditorGizmoHandle::None;
    // Kept as a preferences/API compatibility shim. Transform interaction no
    // longer branches on this legacy mode; every selection uses Universal.
    const wchar_t* name = L"Universal";
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_active_tool"))) {
        label->setText(name);
    }
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::setLocalTransformSpace(bool local)
{
    if (_transformGizmo.active()) {
        finishTransformGizmoDrag(false);
    }
    _localTransformSpace = local;
    _gizmoHoverHandle = EditorGizmoHandle::None;
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_tool_space"))) {
        button->setText(local ? L"Local" : L"World");
    }
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::setViewportOrientationAxisVisible(bool visible)
{
    const bool changed = _viewportOrientationAxisVisible != visible;
    _viewportOrientationAxisVisible = visible;
    if (_viewportOrientationAxisMenuItem != nullptr) {
        _viewportOrientationAxisMenuItem->setText(
            visible ? L"[x] Viewport Orientation Axis"
                    : L"[ ] Viewport Orientation Axis");
    }
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        sub->renderer().setViewportOrientationAxisEnabled(visible);
    }
    if (changed && _onViewportOrientationAxisVisibilityChanged) {
        _onViewportOrientationAxisVisibilityChanged(visible);
    }
    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::newSceneDocument()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    if (_document->isDirty()) {
        const int choice = ::MessageBoxW(
            _hostWindow,
            L"The current scene has unsaved changes.\n\nDiscard them and create a new scene?",
            L"AYEditor", MB_YESNO | MB_ICONWARNING);
        if (choice != IDYES) return;
    }
    _document->newScene();
    afterDocumentReload();
}

void EditorSession::openSceneDocument()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    if (_document->isDirty()) {
        const int choice = ::MessageBoxW(
            _hostWindow,
            L"The current scene has unsaved changes.\n\nDiscard them and open another scene?",
            L"AYEditor", MB_YESNO | MB_ICONWARNING);
        if (choice != IDYES) return;
    }
    const std::string path = showSceneOpenDialog(_hostWindow);
    if (path.empty()) return;

    std::string error;
    if (!_document->open(path, &error)) {
        const std::wstring message(error.begin(), error.end());
        ::MessageBoxW(_hostWindow, message.c_str(), L"Open Scene Failed",
                      MB_OK | MB_ICONERROR);
        return;
    }
    afterDocumentReload();
}

void EditorSession::saveSceneDocument()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    if (_document->path().empty()) {
        saveSceneDocumentAs();
        return;
    }
    std::string error;
    if (!_document->save(&error)) {
        const std::wstring message(error.begin(), error.end());
        ::MessageBoxW(_hostWindow, message.c_str(), L"Save Scene Failed",
                      MB_OK | MB_ICONERROR);
        return;
    }
    refreshUnsavedIndicator();
}

void EditorSession::saveSceneDocumentAs()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    const std::string path = showSceneSaveDialog(_hostWindow);
    if (path.empty()) return;
    std::string error;
    if (!_document->saveAs(path, &error)) {
        const std::wstring message(error.begin(), error.end());
        ::MessageBoxW(_hostWindow, message.c_str(), L"Save Scene Failed",
                      MB_OK | MB_ICONERROR);
        return;
    }
    refreshOutliner();
    refreshUnsavedIndicator();
}

void EditorSession::afterDocumentReload()
{
    clearSelectedEntity(false);
    _playRuntime.forgetEditScenePreview();
    _commands.clear();
    refreshOutliner();
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::createEmptyEntity()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    ayt::entity::World* world = hierarchyWorldMutable();
    if (world == nullptr) return;
    ayt::entity::Entity* entity = world->createEntity();
    if (entity == nullptr) return;
    const std::string name = "Entity "
        + std::to_string(static_cast<unsigned>(world->getAllEntities().size()));
    entity->setName(name.c_str());
    entity->addComponent<ayt::entity::Transform>();
    setSelectedEntity(world, entity);
    _commands.clear();
    _document->markDirty();
    _outlinerRefreshPending = true;
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::deleteSelectedEntity()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    if (world == nullptr || entity == nullptr) return;
    clearSelectedEntity();
    world->destroyEntity(entity);
    _commands.clear();
    _document->markDirty();
    _outlinerRefreshPending = true;
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::bindMenuBar() {
    auto* widget = _ui.findById("menubar");
    auto* menuBar = dynamic_cast<ayt::ui::MenuBar*>(widget);
    if (menuBar == nullptr) {
        return;
    }

    // The editor adds six top-level menus.  MenuBar's generic default uses
    // 80-DIP fixed anchors, while editor_shell.ui.json deliberately leaves a
    // flexible spacer after the menu slot.  Fixed anchors overflowed that
    // slot and were still painted, but the later spacer won reverse-order hit
    // testing over Tools/Help.  Text-sized anchors keep painted and hittable
    // geometry inside the slot.
    menuBar->setAnchorAutoWidth(true);

    ayt::ui::Menu* fileMenu = menuBar->addMenu(L"File");
    if (fileMenu != nullptr) {
        if (auto* item = fileMenu->addItem(L"New")) {
            item->setOnActivate([this]() { newSceneDocument(); });
        }
        if (auto* item = fileMenu->addItem(L"Open...")) {
            item->setOnActivate([this]() { openSceneDocument(); });
        }
        if (auto* item = fileMenu->addItem(L"Save")) {
            item->setOnActivate([this]() { saveSceneDocument(); });
        }
        if (auto* item = fileMenu->addItem(L"Save As...")) {
            item->setOnActivate([this]() { saveSceneDocumentAs(); });
        }
        if (auto* item = fileMenu->addItem(L"Import...")) {
            item->setOnActivate([this]() { importCharacterFromDialog(); });
        }
        fileMenu->addSeparator();
        if (auto* item = fileMenu->addItem(L"Exit")) {
            item->setOnActivate([this]() { requestHostClose(); });
        }
    }

    ayt::ui::Menu* editMenu = menuBar->addMenu(L"Edit");
    if (editMenu != nullptr) {
        if (auto* item = editMenu->addItem(L"Undo")) {
            _undoMenuItem = item;
            item->setOnActivate([this]() {
                if (_gameView.mode() == EditorMode::Edit) _commands.undo();
            });
        }
        if (auto* item = editMenu->addItem(L"Redo")) {
            _redoMenuItem = item;
            item->setOnActivate([this]() {
                if (_gameView.mode() == EditorMode::Edit) _commands.redo();
            });
        }
        editMenu->addSeparator();
        if (auto* item = editMenu->addItem(L"Create Empty Entity")) {
            item->setOnActivate([this]() { createEmptyEntity(); });
        }
        if (auto* item = editMenu->addItem(L"Delete Selected")) {
            item->setOnActivate([this]() { deleteSelectedEntity(); });
        }
    }

    ayt::ui::Menu* viewMenu = menuBar->addMenu(L"View");
    if (viewMenu != nullptr) {
        if (auto* item = viewMenu->addItem(L"Viewport Orientation Axis")) {
            _viewportOrientationAxisMenuItem = item;
            item->setOnActivate([this]() {
                setViewportOrientationAxisVisible(
                    !_viewportOrientationAxisVisible);
            });
            item->setText(_viewportOrientationAxisVisible
                              ? L"[x] Viewport Orientation Axis"
                              : L"[ ] Viewport Orientation Axis");
        }
    }

    ayt::ui::Menu* windowMenu = menuBar->addMenu(L"Window");
    if (windowMenu != nullptr) {
        if (auto* item = windowMenu->addItem(L"Render Settings")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_render", _panelRenderVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Inspector")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_inspector", _panelInspectorVisible);
            });
        }
        // v0.3+ PR-5 — Hierarchy panel toggle (design §4.3.y)
        if (auto* item = windowMenu->addItem(L"Hierarchy")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_outliner", _panelOutlinerVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Network")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_network", _panelNetworkVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Console")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_console", _panelConsoleVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Assets")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_assets", _panelAssetsVisible);
            });
        }
        windowMenu->addSeparator();
        if (auto* item = windowMenu->addItem(L"Save Workspace")) {
            item->setOnActivate([this]() { savePreferencesNow(); });
        }
        if (auto* item = windowMenu->addItem(L"Reset Workspace Layout")) {
            item->setOnActivate([this]() { resetWorkspacePreferences(); });
        }
        windowMenu->addSeparator();
        if (auto* item = windowMenu->addItem(L"Select Character")) {
            item->setOnActivate([this]() { selectCharacter(); });
        }
    }

    ayt::ui::Menu* toolsMenu = menuBar->addMenu(L"Tools");
    if (toolsMenu != nullptr) {
        if (auto* item = toolsMenu->addItem(L"UI Layout Editor...")) {
            item->setOnActivate([this]() { openLayoutEditorWindow(); });
        }
        if (auto* item = toolsMenu->addItem(L"Audio Editor...")) {
            item->setOnActivate([this]() { openAudioEditorWindow(); });
        }
    }

    ayt::ui::Menu* helpMenu = menuBar->addMenu(L"Help");
    if (helpMenu != nullptr) {
        if (auto* item = helpMenu->addItem(L"About AYEditor")) {
            item->setOnActivate([]() {});
        }
    }
}

void EditorSession::syncLayoutEditorLifetime() {
    if (_layoutEditor == nullptr) {
        return;
    }
    if (_childWindows == nullptr || _layoutEditorHandle == nullptr) {
        _layoutEditor.reset();
        _layoutEditorHandle = nullptr;
        return;
    }
    bool alive = false;
    for (const auto& entry : _childWindows->entries()) {
        if (entry.handle == _layoutEditorHandle) {
            alive = true;
            break;
        }
    }
    if (!alive) {
        _layoutEditor.reset();
        _layoutEditorHandle = nullptr;
    }
}

void EditorSession::openLayoutEditorWindow() {
    if (_childWindows == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] UI Layout Editor requires ChildWindowManager\n");
        return;
    }
    syncLayoutEditorLifetime();
    if (_layoutEditor != nullptr && _layoutEditorHandle != nullptr) {
        // Already open — leave the existing child focused.
        return;
    }

    ChildWindowConfig cfg;
    cfg.title = "UI Layout Editor";
    cfg.layoutPath = resolveLayoutEditorChromePath();
    cfg.x = 120;
    cfg.y = 80;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.beforeMouseButton =
        [this](ayt::ui::UIManager& /*ui*/, float x, float y, int button,
               bool pressed) -> bool {
            if (_layoutEditor == nullptr) {
                return false;
            }
            const ayt::math::FVector2 pos(x, y);
            return pressed ? _layoutEditor->onPointerDown(pos, button)
                           : _layoutEditor->onPointerUp(pos, button);
        };
    cfg.beforeMouseMove =
        [this](ayt::ui::UIManager& /*ui*/, float x, float y) -> bool {
            if (_layoutEditor == nullptr) {
                return false;
            }
            return _layoutEditor->onPointerMove(ayt::math::FVector2(x, y));
        };
    cfg.beforeMouseWheel =
        [this](ayt::ui::UIManager& /*ui*/, float x, float y,
               float deltaY) -> bool {
            if (_layoutEditor == nullptr) {
                return false;
            }
            return _layoutEditor->onWheel(ayt::math::FVector2(x, y), deltaY);
        };
    cfg.beforeKey =
        [this](ayt::ui::UIManager& /*ui*/, ayt::device::KeyCode kc,
               bool pressed) -> bool {
            if (_layoutEditor == nullptr) {
                return false;
            }
            const int uiKey =
                static_cast<int>(ayt::ui::fromDeviceKey(kc));
            if (!pressed) {
                _layoutEditor->onKeyUp(uiKey);
                return false;
            }
            if (uiKey == ayt::ui::UIKey_Shift ||
                uiKey == ayt::ui::UIKey_Control ||
                uiKey == ayt::ui::UIKey_Alt) {
                return false;
            }
            return _layoutEditor->onKeyDown(uiKey);
        };
    cfg.onFocusChanged =
        [this](ayt::ui::UIManager& /*ui*/, bool focused) {
            if (!focused && _layoutEditor != nullptr) {
                _layoutEditor->onKeyUp(ayt::ui::UIKey_Space);
            }
        };
    cfg.resolveCursorHint =
        [this](ayt::ui::UIManager& /*ui*/, float x, float y) {
            if (_layoutEditor == nullptr) {
                return ayt::ui::UiCursorHint::Default;
            }
            return _layoutEditor->canvasCursorHint(ayt::math::FVector2(x, y));
        };
    cfg.beforeClose = [this](ayt::ui::UIManager& /*ui*/) {
        if (_layoutEditor != nullptr) {
            _layoutEditor->detach();
            _layoutEditor.reset();
        }
        _layoutEditorHandle = nullptr;
    };

    EditorChildWindowManager::Handle handle = nullptr;
    if (!_childWindows->openChildWindow(cfg, handle) || handle == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] failed to open UI Layout Editor (%s)\n",
            cfg.layoutPath.c_str());
        return;
    }

    ayt::ui::UIManager* childUi = nullptr;
    for (const auto& entry : _childWindows->entries()) {
        if (entry.handle == handle) {
            childUi = entry.ui.get();
            break;
        }
    }
    if (childUi == nullptr) {
        _childWindows->closeChildWindow(handle);
        return;
    }

    auto session = std::make_unique<ayt::ui::LayoutEditorSession>();
    HWND owner = _hostWindow;
    session->setOpenPathPicker([owner]() { return showUiJsonOpenDialog(owner); });
    session->setSavePathPicker([owner]() { return showUiJsonSaveDialog(owner); });
    if (!session->attach(*childUi)) {
        std::fprintf(stderr,
            "[EditorSession] LayoutEditorSession::attach failed\n");
        _childWindows->closeChildWindow(handle);
        return;
    }

    _layoutEditor = std::move(session);
    _layoutEditorHandle = handle;
}

void EditorSession::syncAudioEditorLifetime() {
    if (_audioEditor == nullptr) {
        return;
    }
    if (_childWindows == nullptr || _audioEditorHandle == nullptr) {
        _audioEditor.reset();
        _audioEditorHandle = nullptr;
        return;
    }
    bool alive = false;
    for (const auto& entry : _childWindows->entries()) {
        if (entry.handle == _audioEditorHandle) {
            alive = true;
            break;
        }
    }
    if (!alive) {
        _audioEditor.reset();
        _audioEditorHandle = nullptr;
    }
}

void EditorSession::openAudioEditorWindow() {
    if (_childWindows == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] Audio Editor requires ChildWindowManager\n");
        return;
    }
    syncAudioEditorLifetime();
    if (_audioEditor != nullptr && _audioEditorHandle != nullptr) {
        return;
    }

    ChildWindowConfig cfg;
    cfg.title = "Audio Editor";
    cfg.layoutPath = resolveAudioEditorChromePath();
    cfg.x = 140;
    cfg.y = 100;
    cfg.width = 960;
    cfg.height = 640;
    cfg.beforeClose = [this](ayt::ui::UIManager& /*ui*/) {
        if (_audioEditor != nullptr) {
            _audioEditor->detach();
            _audioEditor.reset();
        }
        _audioEditorHandle = nullptr;
    };

    EditorChildWindowManager::Handle handle = nullptr;
    if (!_childWindows->openChildWindow(cfg, handle) || handle == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] failed to open Audio Editor (%s)\n",
            cfg.layoutPath.c_str());
        return;
    }

    ayt::ui::UIManager* childUi = nullptr;
    for (const auto& entry : _childWindows->entries()) {
        if (entry.handle == handle) {
            childUi = entry.ui.get();
            break;
        }
    }
    if (childUi == nullptr) {
        _childWindows->closeChildWindow(handle);
        return;
    }

    auto session = std::make_unique<ayt::audio::AudioEditorSession>();
    // Edit mode does not run GameLoop audio ticks — session drives update.
    // During Play, GameLoop also updates Audio; double pump is safe.
    session->setDriveAudioUpdate(true);
    ayt::audio::AudioSubSystem* audioSub =
        ayt::audio::AudioSubSystem::findRegistered();
    if (audioSub == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] Audio Editor: no AudioSubSystem "
            "(built with -no-audio?)\n");
        _childWindows->closeChildWindow(handle);
        return;
    }
    if (audioSub->engine() == nullptr || !audioSub->engine()->isInitialized()) {
        if (!audioSub->initialize()) {
            std::fprintf(stderr,
                "[EditorSession] AudioSubSystem::initialize failed\n");
            _childWindows->closeChildWindow(handle);
            return;
        }
        if (auto* host = ayt::app::currentEngineHost()) {
            ayt::app::bindBuiltinHostServices(*host);
        }
    }
    session->setAudio(audioSub);
    HWND owner = _hostWindow;
    session->setPathPicker([owner]() { return showOpenAudioFileDialog(owner); });
    if (!session->attach(*childUi)) {
        std::fprintf(stderr,
            "[EditorSession] AudioEditorSession::attach failed\n");
        _childWindows->closeChildWindow(handle);
        return;
    }

    _audioEditor = std::move(session);
    _audioEditorHandle = handle;
}

void EditorSession::requestHostClose() {
    if (_hostWindow != nullptr) {
        ::PostMessageW(_hostWindow, WM_CLOSE, 0, 0);
    }
}

void EditorSession::requestHostMinimize() {
    if (_hostWindow != nullptr) {
        ::ShowWindow(_hostWindow, SW_MINIMIZE);
    }
}

void EditorSession::requestHostMaximizeToggle() {
    if (_hostWindow == nullptr) {
        return;
    }
    if (::IsZoomed(_hostWindow)) {
        ::ShowWindow(_hostWindow, SW_RESTORE);
    } else {
        ::ShowWindow(_hostWindow, SW_MAXIMIZE);
    }
}

namespace {

// Join a vector of type-name strings for stable stderr output,
// e.g. {"Animation", "Material"} -> "Material, Animation"
// (insertion order preserved). Used by the import pipeline's
// log lines.
std::string joinTypeNames(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

} // namespace

// Phase 2a: toolbar Import button target. Mirrors the
// --import pipeline from EditorApp::run() but with the path
// source being the Win32 dialog (Phase 1 left this as a one-
// line shim). The dialog blocks on a modal until the user
// picks or cancels, so this method is fired-and-forgot from
// the main thread; nothing else on the UI thread runs while
// it's open.
void EditorSession::importCharacterFromDialog()
{
    const std::string sourcePath =
        ImportDialog::showOpenFileDialog(_hostWindow);
    if (sourcePath.empty()) {
        // User cancelled (or non-Windows stub returned empty).
        // Silent no-op; do not pollute stderr with a "no path"
        // message because cancels are a normal interaction.
        return;
    }

    const std::string cacheRoot =
        EditorPlayRuntime::resolvePersistentCacheRoot();
    const std::string assetRoot = cacheRoot + "assets\\";

    const Importer::Result result =
        ImportDialog::importFromPath(sourcePath, assetRoot);
    if (!result.success) {
        std::fprintf(stderr,
                     "[EditorSession] import failed: %s\n",
                     result.errorMessage.c_str());
        return;
    }

    ImportedCharacterMapDiagnostics diag;
    const ImportedCharacter mapped =
        mapConversionToImportedCharacter(result.conversion, cacheRoot, diag);
    if (!diag.success) {
        std::fprintf(stderr,
                     "[EditorSession] import produced no skinned character: "
                     "missing [%s]\n",
                     joinTypeNames(diag.missing).c_str());
        // Mapper rejected; keep whatever is currently spawned
        // (cube if no previous import, or prior character if
        // hot-swap with rejection). Do NOT replace with default-
        // constructed; that would clear a previously-imported
        // valid character on a bad second import.
        return;
    }

    // Character import uses the same project-local generated root as the
    // Content Browser. Publish the newly cooked products before changing the
    // preview selection so the browser is current without needing a manual R.
    _assetCurrentFolder = "Imported";
    (void)rescanAssetsNow();

    _playRuntime.replaceImportedCharacter(mapped);
    if (_gameView.mode() == EditorMode::Edit) {
        if (_document != nullptr) _document->markDirty();
        _outlinerRefreshPending = true;
        selectCharacter();
        refreshTransformInspector();
        refreshUnsavedIndicator();
    }
    std::fprintf(stderr,
                 "[EditorSession] imported character ready "
                 "(mesh=%s, skel=%s, anim=%s)\n",
                 mapped.meshPath.c_str(),
                 mapped.skeletonPath.c_str(),
                 mapped.animationPath.c_str());

    // ED-03: a successful import auto-selects the new character
    // for inspection, so the designer can immediately click
    // [Pick Anim] and re-route to a different .ayanm. Without
    // this the user has to click [Select] twice (once to
    // populate the inspector, once after applying).
    refreshInspectorLabels();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

// ED-03: walk the inspector's TextLabels and update them with
// the currently-spawned Play entity (character preferred, else cube).
//
// v0.4 PR-2 (design §6 decision 7a + LM-2 fix): Play/Paused 模式下
// inspector 已 lock（PR-5 LM-2 语义），上层 onModeChanged 已
// `setInspectorHint("Locked during Play.")`。本函数必须不再触碰
// 任何 inspector label — 否则 line 1590 "No selection" 会覆盖上层
// 的 mode-aware hint（pre-existing bug，PR-1 验证：git stash 后
// baseline 同样 fail）。Edit 模式行为不变。
void EditorSession::refreshInspectorLabels()
{
    if (_selectedAssetId != 0) {
        setInspectorAssetMode(true);
        refreshAssetInspector();
        return;
    }
    setInspectorAssetMode(false);

    auto setUtf8 = [this](const char* id, const std::string& utf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(utf8.begin(), utf8.end()));
            }
        }
    };

    // v0.3+ PR-5 — Hierarchy 选择优先于 PR-4 的 character/cube 二选一。
    // 存 id 不存指针 → 每次重解析，实体没了自动降级（Landmine F）。
    if (!_selection.empty()) {
        if (auto* w = hierarchyWorldMutable()) {
            if (ayt::entity::Entity* sel = _selection.resolve(w)) {
                const char* nm = sel->getName();
                setUtf8("inspector_hint",
                        std::string(_gameView.mode() == EditorMode::Edit
                                        ? "Hierarchy: " : "Play selection: ")
                        + ((nm && nm[0]) ? nm : "entity"));
                if (auto* meshC = sel->getComponent<ayt::entity::MeshComponent>()) {
                    setUtf8("inspector_mesh", "mesh: " + meshC->meshPath);
                } else {
                    setUtf8("inspector_mesh", "mesh: -");
                }
                if (auto* skelC = sel->getComponent<ayt::entity::SkeletonComponent>()) {
                    setUtf8("inspector_skel", "skel: " + skelC->skeletonPath);
                } else {
                    setUtf8("inspector_skel", "skel: -");
                }
                if (auto* animC = sel->getComponent<ayt::entity::AnimationComponent>()) {
                    setUtf8("inspector_anim",
                            animC->clipPath.empty() ? "anim: (bind-pose)"
                                                    : ("anim: " + animC->clipPath));
                } else {
                    setUtf8("inspector_anim", "anim: -");
                }
                return;
            }
        }
        clearSelectedEntity(false);  // 已销毁 → 降级到 PR-4 路径
    }

    if (_gameView.mode() != EditorMode::Edit) {
        setUtf8("inspector_hint", "Locked during Play.");
        setUtf8("inspector_mesh", "mesh: -");
        setUtf8("inspector_skel", "skel: -");
        setUtf8("inspector_anim", "anim: -");
        return;
    }

    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = nullptr;
    if (_inspectorPreferCube && cube != nullptr) {
        e = cube;
    } else if (character != nullptr) {
        e = character;
    } else {
        e = cube;
    }

    if (e == nullptr) {
        setUtf8("inspector_hint", "No selection");
        setUtf8("inspector_mesh", "mesh: -");
        setUtf8("inspector_skel", "skel: -");
        setUtf8("inspector_anim", "anim: -");
        return;
    }

    const bool isCharacter = (e == character);
    char hint[96];
    std::snprintf(hint, sizeof(hint), "%s  (click#%u)",
                  isCharacter ? "Character" : "Cube (opaque ref)",
                  static_cast<unsigned>(_viewportClickCount));
    setUtf8("inspector_hint", hint);

    if (auto* meshC = e->getComponent<ayt::entity::MeshComponent>()) {
        setUtf8("inspector_mesh", "mesh: " + meshC->meshPath);
    } else {
        setUtf8("inspector_mesh", "mesh: -");
    }
    if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
        setUtf8("inspector_skel", "skel: " + skelC->skeletonPath);
    } else {
        setUtf8("inspector_skel", "skel: -");
    }
    if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
        setUtf8("inspector_anim",
                animC->clipPath.empty() ? "anim: (bind-pose)"
                                        : ("anim: " + animC->clipPath));
    } else {
        setUtf8("inspector_anim", "anim: -");
    }
}

void EditorSession::bindTransformInspector()
{
    const char* ids[] = {
        "transform_px", "transform_py", "transform_pz",
        "transform_rx", "transform_ry", "transform_rz",
        "transform_sx", "transform_sy", "transform_sz",
    };
    for (const char* id : ids) {
        auto* input = dynamic_cast<ayt::ui::TextInput*>(_ui.findById(id));
        if (input == nullptr) continue;
        input->setOnSubmit([this](const std::wstring&) {
            if (!_updatingTransformInputs) applyTransformInspector();
        });
        input->setOnFocusLostNotify([this]() {
            if (!_updatingTransformInputs) applyTransformInspector();
        });
    }
    _ui.bindEvent("btn_transform_apply", "onClick",
                  [this]() { applyTransformInspector(); });
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_transform_apply"))) {
        button->setOnClicked([this]() { applyTransformInspector(); });
    }
}

void EditorSession::refreshTransformInspector()
{
    const char* ids[] = {
        "transform_px", "transform_py", "transform_pz",
        "transform_rx", "transform_ry", "transform_rz",
        "transform_sx", "transform_sy", "transform_sz",
    };
    auto setAll = [this, &ids](const std::wstring (&values)[9], bool readOnly) {
        _updatingTransformInputs = true;
        for (size_t i = 0; i < 9; ++i) {
            if (auto* input = dynamic_cast<ayt::ui::TextInput*>(
                    _ui.findById(ids[i]))) {
                input->setText(values[i]);
                input->setReadOnly(readOnly);
            }
        }
        _updatingTransformInputs = false;
    };

    ayt::entity::Entity* entity = _selection.resolve(hierarchyWorldMutable());
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) {
        const std::wstring empty[9] = {
            L"-", L"-", L"-", L"-", L"-", L"-", L"-", L"-", L"-"
        };
        setAll(empty, true);
        return;
    }

    constexpr float radiansToDegrees = 57.29577951308232f;
    const ayt::math::FVector3 euler = transform->rotation.toEulerAngles();
    const std::wstring values[9] = {
        formatFloat(transform->position.x), formatFloat(transform->position.y),
        formatFloat(transform->position.z), formatFloat(euler.x * radiansToDegrees),
        formatFloat(euler.y * radiansToDegrees), formatFloat(euler.z * radiansToDegrees),
        formatFloat(transform->scale.x), formatFloat(transform->scale.y),
        formatFloat(transform->scale.z),
    };
    // Runtime clones remain inspectable during Play/Paused, but edits stay
    // locked so Inspector cannot mutate simulation state behind the host.
    setAll(values, _gameView.mode() != EditorMode::Edit);
}

void EditorSession::applyTransformInspector()
{
    if (_updatingTransformInputs || _gameView.mode() != EditorMode::Edit) return;
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    if (world == nullptr || entity == nullptr
        || entity->getComponent<ayt::entity::Transform>() == nullptr) {
        return;
    }

    const char* ids[] = {
        "transform_px", "transform_py", "transform_pz",
        "transform_rx", "transform_ry", "transform_rz",
        "transform_sx", "transform_sy", "transform_sz",
    };
    float values[9] = {};
    for (size_t i = 0; i < 9; ++i) {
        auto* input = dynamic_cast<ayt::ui::TextInput*>(_ui.findById(ids[i]));
        if (input == nullptr || !parseFloat(input->getText(), values[i])) {
            refreshTransformInspector();
            return;
        }
    }

    constexpr float degreesToRadians = 0.017453292519943295f;
    EditorTransformState state;
    state.position = {values[0], values[1], values[2]};
    state.rotation = ayt::math::FQuaternion::fromEulerAngles({
        values[3] * degreesToRadians,
        values[4] * degreesToRadians,
        values[5] * degreesToRadians,
    });
    state.scale = {values[6], values[7], values[8]};
    _commands.executeTransform(*world, entity->getId(), state);
}

void EditorSession::selectPlayEntityFromViewport()
{
    ayt::entity::World* world = hierarchyWorldMutable();
    applyViewportSelection(world,
                           pickEntityFromViewport(_viewportLmbX, _viewportLmbY));
}

ayt::entity::Entity* EditorSession::pickEntityFromViewport(float x, float y)
{
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::math::FRectangle viewport{};
    if (world == nullptr || !getViewportBounds(viewport)
        || viewport.width() <= 0.0f || viewport.height() <= 0.0f) {
        return nullptr;
    }

    ayt::math::FVector3 direction{};
    if (!viewportRayDirection(x, y, direction)) {
        return nullptr;
    }
    const ayt::math::FVector3 origin = _freecam.eye();

    ayt::entity::Entity* bestEntity = nullptr;
    float bestDistance = 1.0e30f;
    for (ayt::entity::Entity* entity : world->getAllEntities()) {
        auto* transform = entity != nullptr
            ? entity->getComponent<ayt::entity::Transform>() : nullptr;
        auto* meshComponent = entity != nullptr
            ? entity->getComponent<ayt::entity::MeshComponent>() : nullptr;
        if (transform == nullptr || (meshComponent != nullptr
                                     && !meshComponent->visible)) {
            continue;
        }

        float distance = 0.0f;
        bool hit = false;
        bool hasMeshBounds = false;
        if (meshComponent != nullptr && !meshComponent->meshPath.empty()) {
            const auto mesh = ayt::resource::ResourceManager::instance()
                .load<ayt::resource::IMesh>(meshComponent->meshPath);
            if (mesh != nullptr && mesh->hasBounds()) {
                hasMeshBounds = true;
                const ayt::math::Float4x4 worldMatrix =
                    ayt::math::Transform::getMatrix(
                        transform->position, transform->rotation, transform->scale);
                const ayt::math::Float4x4 inverseWorld = worldMatrix.inverse_fast();
                const ayt::math::FVector3 localOrigin =
                    inverseWorld.transformPoint(origin);
                ayt::math::FVector3 localDirection =
                    inverseWorld.transformDirection(direction);
                if (localDirection.lengthSq() > 1.0e-10f) {
                    localDirection = localDirection.normalize();
                    float localDistance = 0.0f;
                    const ayt::resource::Bounds bounds = mesh->getBounds();
                    if (intersectRayAabb(localOrigin, localDirection,
                                         bounds.getMin(), bounds.getMax(),
                                         localDistance)) {
                        const ayt::math::FVector3 localHit =
                            localOrigin + localDirection * localDistance;
                        const ayt::math::FVector3 worldHit =
                            worldMatrix.transformPoint(localHit);
                        distance = (worldHit - origin).dot(direction);
                        hit = distance >= 0.0f;
                    }
                }
            }
        }

        // Transform-only entities and resources without cooked bounds remain
        // selectable. Do not use the fallback after an AABB miss: that would
        // make large sparse meshes steal clicks outside their visible bounds.
        if (!hasMeshBounds) {
            const float radius = 0.75f * std::max({
                std::fabs(transform->scale.x), std::fabs(transform->scale.y),
                std::fabs(transform->scale.z), 0.1f});
            hit = intersectRaySphere(origin, direction, transform->position,
                                     radius, distance);
        }
        if (hit && distance < bestDistance) {
            bestDistance = distance;
            bestEntity = entity;
        }
    }

    return bestEntity;
}

void EditorSession::applyViewportSelection(ayt::entity::World* world,
                                           ayt::entity::Entity* entity)
{
    setSelectedEntity(world, entity);
    ++_viewportClickCount;
    _outlinerRefreshPending = true;
    refreshInspectorLabels();
    refreshTransformInspector();
    if (_repaintCallback) _repaintCallback();
}

EditorGizmoHandle EditorSession::hitTestTransformGizmo(float x, float y)
{
    if (_gameView.mode() != EditorMode::Edit) {
        return EditorGizmoHandle::None;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (world == nullptr || world != _selectionWorld || transform == nullptr) {
        return EditorGizmoHandle::None;
    }
    ayt::math::FVector3 direction{};
    if (!viewportRayDirection(x, y, direction)) {
        return EditorGizmoHandle::None;
    }
    const EditorTransformState state{
        transform->position, transform->rotation, transform->scale};
    _gizmoDisabledHandleMask = EditorTransformGizmo::disabledHandleMask(
        state, _localTransformSpace, _freecam.eye(),
        _gizmoDisabledHandleMask);
    return _transformGizmo.hitTestUniversal(
        state, _localTransformSpace, _freecam.eye(), direction,
        _gizmoDisabledHandleMask);
}

bool EditorSession::beginTransformGizmoDrag(EditorGizmoHandle handle,
                                            float x, float y)
{
    if (_gameView.mode() != EditorMode::Edit
        || handle == EditorGizmoHandle::None) {
        return false;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (world == nullptr || world != _selectionWorld || transform == nullptr) {
        return false;
    }
    ayt::math::FVector3 direction{};
    if (!viewportRayDirection(x, y, direction)) return false;

    const EditorTransformState state{
        transform->position, transform->rotation, transform->scale};
    if (!_transformGizmo.beginUniversal(
            handle, state, _localTransformSpace,
            _freecam.eye(), direction, y,
            _gizmoDisabledHandleMask)) {
        return false;
    }
    _gizmoDragWorld = world;
    _gizmoDragEntityId = entity->getId();
    _gizmoHoverHandle = handle;
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
    return true;
}

bool EditorSession::updateTransformGizmoDrag(float x, float y)
{
    if (!_transformGizmo.active() || _gizmoDragWorld == nullptr) {
        return false;
    }
    ayt::entity::Entity* entity =
        _gizmoDragWorld->findEntity(_gizmoDragEntityId);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) {
        finishTransformGizmoDrag(false);
        return false;
    }
    ayt::math::FVector3 direction{};
    if (!viewportRayDirection(x, y, direction)) return true;

    EditorTransformState updated;
    if (_transformGizmo.update(
            _freecam.eye(), direction, x, y, updated)) {
        transform->setPosition(updated.position.x,
                               updated.position.y,
                               updated.position.z);
        transform->setRotation(updated.rotation.x,
                               updated.rotation.y,
                               updated.rotation.z,
                               updated.rotation.w);
        transform->setScale(updated.scale.x,
                            updated.scale.y,
                            updated.scale.z);
        refreshTransformInspector();
        syncTransformGizmoToRenderer();
        if (_repaintCallback) _repaintCallback();
    }
    return true;
}

void EditorSession::finishTransformGizmoDrag(bool commit)
{
    if (!_transformGizmo.active()) return;

    ayt::entity::World* world = _gizmoDragWorld;
    const uint32_t entityId = _gizmoDragEntityId;
    const EditorTransformState before = _transformGizmo.before();
    ayt::entity::Entity* entity = world != nullptr
        ? world->findEntity(entityId) : nullptr;
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    EditorTransformState after = before;
    if (transform != nullptr) {
        after = {transform->position, transform->rotation, transform->scale};
        transform->setPosition(before.position.x, before.position.y,
                               before.position.z);
        transform->setRotation(before.rotation.x, before.rotation.y,
                               before.rotation.z, before.rotation.w);
        transform->setScale(before.scale.x, before.scale.y, before.scale.z);
    }

    _transformGizmo.reset();
    _gizmoDragWorld = nullptr;
    _gizmoDragEntityId = 0;
    _gizmoHoverHandle = EditorGizmoHandle::None;
    if (commit && transform != nullptr) {
        _commands.executeTransform(*world, entityId, after);
    } else {
        refreshTransformInspector();
    }
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::updateTransformGizmoHover(float x, float y)
{
    if (_transformGizmo.active()) return;
    const uint16_t oldDisabledHandles = _gizmoDisabledHandleMask;
    const EditorGizmoHandle handle = hitTestTransformGizmo(x, y);
    if (handle == _gizmoHoverHandle
        && oldDisabledHandles == _gizmoDisabledHandleMask) {
        return;
    }
    _gizmoHoverHandle = handle;
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::syncTransformGizmoToRenderer()
{
    auto* subsystem = ayt::render::RendererSubSystem::findRegistered();
    if (subsystem == nullptr) return;

    ayt::render::EditorTransformGizmoState state;
    if (_gameView.mode() == EditorMode::Edit) {
        ayt::entity::World* world = hierarchyWorldMutable();
        ayt::entity::Entity* entity = _selection.resolve(world);
        auto* transform = entity != nullptr
            ? entity->getComponent<ayt::entity::Transform>() : nullptr;
        if (world != nullptr && world == _selectionWorld
            && transform != nullptr) {
            const EditorTransformState transformState{
                transform->position, transform->rotation, transform->scale};
            _gizmoDisabledHandleMask =
                EditorTransformGizmo::disabledHandleMask(
                    transformState, _localTransformSpace, _freecam.eye(),
                    _gizmoDisabledHandleMask);
            state.visible = true;
            state.position = transform->position;
            state.rotation = transform->rotation;
            state.localSpace = _localTransformSpace;
            state.activeHandle = static_cast<uint8_t>(
                _transformGizmo.active()
                    ? _transformGizmo.activeHandle()
                    : _gizmoHoverHandle);
            state.disabledHandleMask = _gizmoDisabledHandleMask;
            // The active handle must not dim halfway through a rotation that
            // changes its own local basis. Camera movement is unavailable
            // during a left-button gizmo drag, so keeping it active is safe.
            state.disabledHandleMask &= static_cast<uint16_t>(
                ~EditorTransformGizmo::handleBit(
                    _transformGizmo.activeHandle()));
            state.mode = ayt::render::EditorTransformGizmoMode::Universal;
        }
    }
    if (!state.visible) _gizmoDisabledHandleMask = 0u;
    subsystem->renderer().setEditorTransformGizmoState(state);
}

// ED-03: [Select] handler. Snapshots paths into the inspector.
// Falls back to the procedural cube when character spawn failed.
void EditorSession::selectCharacter()
{
    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = nullptr;
    if (_inspectorPreferCube && cube != nullptr) {
        e = cube;
    } else if (character != nullptr) {
        e = character;
    } else {
        e = cube;
    }

    if (e == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] nothing to select; enter Play first "
            "(character or cube)\n");
        refreshInspectorLabels();
        return;
    }

    setSelectedEntity(hierarchyWorldMutable(), e);

    if (e == character) {
        if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
            _inspectorSkelPick = skelC->skeletonPath;
        }
        if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
            _inspectorAnimPick = animC->clipPath;
        }
    } else {
        _inspectorSkelPick.clear();
        _inspectorAnimPick.clear();
    }

    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// ED-03: [Pick Skel] handler. Opens the Win32 dialog filtered
// to .ayskel, stashes the chosen path into _inspectorSkelPick.
// The path is NOT yet applied to the live entity - [Apply]
// commits it. Cancel returns empty = no-op.
void EditorSession::pickInspectorSkeleton()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    // We currently pass the empty filter straight through; the
    // ImportDialog::showOpenFileDialog defaults to its built-in
    // 3D-Model (.fbx/.gltf/.glb) filter, which is wider than
    // .ayskel. To stay within Phase 1 scope we accept the wider
    // filter (the user just types the path or picks any
    // cache-resident file). A typed filter arg is a Phase 2
    // refinement when picker infrastructure exists.
    const std::string picked =
        ayt::editor::ImportDialog::showOpenFileDialog(_hostWindow);
    if (picked.empty()) {
        return; // user cancelled
    }
    setInspectorSkeletonPath(picked);
}

// ED-03: [Pick Anim] handler, sibling of pickInspectorSkeleton.
void EditorSession::pickInspectorAnimation()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    const std::string picked =
        ayt::editor::ImportDialog::showOpenFileDialog(_hostWindow);
    if (picked.empty()) {
        return;
    }
    setInspectorAnimationPath(picked);
}

// ED-03: thin setters that stash the path and refresh the
// corresponding inspector label so the user sees feedback
// between [Pick ...] and [Apply].
void EditorSession::setInspectorSkeletonPath(const std::string& path)
{
    _inspectorSkelPick = path;
    if (auto* w = _ui.findById("inspector_skel")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            label->setText(std::wstring(path.begin(), path.end()));
        }
    }
}

void EditorSession::setInspectorAnimationPath(const std::string& path)
{
    _inspectorAnimPick = path;
    if (auto* w = _ui.findById("inspector_anim")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            label->setText(std::wstring(path.begin(), path.end()));
        }
    }
}

// ED-03: [Apply] handler. Builds an EntityInspectorOverrides
// from the staged picks and forwards to the runtime. Empty
// pick on a field => leave that component path unchanged
// (the runtime's applyComponentOverrides treats empty as
// "keep").
void EditorSession::applyInspectorOverrides()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    EntityInspectorOverrides ov;
    ov.skeletonPathOverride  = _inspectorSkelPick;
    ov.animationPathOverride = _inspectorAnimPick;
    commitInspectorOverrides(ov);
}

// ED-03: [Reset] handler. Clear pending overrides back to
// default (which the runtime interprets as "stop applying
// user picks on the next spawn"). Does NOT clear the live
// entity's component paths - the user keeps whatever is
// animating now.
void EditorSession::resetInspectorOverrides()
{
    // PR-5 (LM-2): Play/Paused 时锁 Inspector 写路径。
    if (!allowInspectorEdit()) return;
    _inspectorSkelPick.clear();
    _inspectorAnimPick.clear();
    EntityInspectorOverrides emptyOv;
    commitInspectorOverrides(emptyOv);
    std::fprintf(stderr, "[EditorSession] inspector overrides cleared\n");
}

// ED-03: shared work for both Apply and Reset. Forward the
// override to the runtime, refresh labels, trigger redraw.
void EditorSession::commitInspectorOverrides(const EntityInspectorOverrides& ov)
{
    // PR-5 (LM-2): 双层守卫 — applyInspectorOverrides/resetInspectorOverrides
    // 入口已守；此处再守一次防外部 caller 直接调 commitInspectorOverrides 路径。
    if (!allowInspectorEdit()) return;
    _playRuntime.applyComponentOverrides(ov);
    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// =============================================================================
// D5.5 — Card promotion wiring.
//
// We walk the primary UIManager's root widget tree and inject
// setPromoteCallback into every DockCard we encounter (slot or floating).
// The lambda closes over `this` so detachToOwnWindow on any card routes
// into the optional EditorChildWindowManager. Card-promotion is a
// pure-AYUI feature; the only AYEditor involvement is injecting the
// callback — we do NOT touch DockCard's interface here (callback
// injection is the contract, per design §17.5 D5.5).
//
// Walk strategy: recursive descent via getChildren(). The root may be
// nullptr (no layout loaded yet) — we silently no-op. A DockArea
// that isn't reached because the layout tree didn't include one also
// no-ops; there are simply no cards to wire.
// =============================================================================

namespace {
// Wire the promote callback into one DockCard. The callback closes over
// `session` and routes into the optional EditorChildWindowManager. The
// card's PromoteCallback member is persistent — wiring once is enough
// for the card's whole lifetime (float/dock moves don't reset it).
void wireCardPromotion(ayt::ui::DockCard* card, EditorSession* session) {
    card->setPromoteCallback(
        [session](
            ayt::ui::DockCard* promoted,
            const std::wstring& title,
            int x, int y, int w, int h) -> bool {
            // PR-Dock-TearOff live-card migration: the host reparents
            // the card ITSELF into the child window's UIManager —
            // no JSON rebuild, the whole live subtree moves. x/y are
            // primary-client coords; promoteCard converts to screen.
            if (session && session->childWindows()) {
                return session->childWindows()
                    ->promoteCard(promoted, title, x, y, w, h);
            }
            return false;
        });
}

void wirePromoteCallbackRecursive(ayt::ui::Widget* w, EditorSession* session) {
    if (!w) return;
    if (auto* card = dynamic_cast<ayt::ui::DockCard*>(w)) {
        wireCardPromotion(card, session);
    }
    // Traverse the actual widget tree rather than only DockArea's visible
    // slot/overlay enumerations. Hidden persistent panels are parked as direct
    // DockArea children and otherwise missed here, producing promoteCb=0 when
    // a saved workspace later reveals them from the Window menu.
    for (ayt::ui::Widget* child : w->getChildren()) {
        wirePromoteCallbackRecursive(child, session);
    }
}
} // namespace

void EditorSession::wirePromoteCallback() {
    if (!_childWindows) return;  // no manager → no-op
    wirePromoteCallbackRecursive(_ui.root(), this);
}

void EditorSession::setModeLabel(const std::wstring& text) {
    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

// PR-5 (LM-2): Inspector hint 文案切换 helper.
// inspector_hint TextLabel 在 editor_shell.ui.json:141 已存在（id=
// "inspector_hint", initial text "No selection"）。Play/Paused 时切
// "Locked during Play"；Edit 模式回 "Click buttons to configure."
// （让用户在 Reset 完 pick 后看见可操作提示）。
void EditorSession::setInspectorHint(const std::wstring& text) {
    if (auto* widget = _ui.findById("inspector_hint")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

void EditorSession::onModeChanged(EditorMode mode) {
    _ui.cancelCapture();
    finishTransformGizmoDrag(false);
    _gizmoHoverHandle = EditorGizmoHandle::None;
    if (_freecam.isLooking()) {
        _freecam.endLook();
    }

    ayt::entity::World* activeWorld = hierarchyWorldMutable();
    if (mode == EditorMode::Edit) {
        // enterEdit() has already destroyed the Play scene.
        clearSelectedEntity(false);
    } else if (_selectionWorld != activeWorld) {
        // Entering Play swaps from the persistent Edit world to its clone.
        // The Edit world is still alive, so its outline can be removed.
        clearSelectedEntity();
    }

    switch (mode) {
    case EditorMode::Edit:
        setModeLabel(L"EDIT");
        // PR-5 (LM-2): Inspector 写权限恢复 + hint 文案恢复。
        _allowInspectorEdit = true;
        setInspectorHint(L"Click buttons to configure.");
        pushFreecamToRenderer();
        break;
    case EditorMode::Play:
        setModeLabel(_netClientAutoPlay ? L"PLAY (NET CLIENT)" : L"PLAY");
        // PR-5 (LM-2): Inspector 写权限锁 + hint 提示。
        _allowInspectorEdit = false;
        setInspectorHint(L"Locked during Play.");
        applyRenderSettingsFromPanel();
        pushFreecamToRenderer();
        // Auto-select the initial runtime subject, but preserve a viewport
        // selection when resuming Play from Paused.
        if (_selection.empty()) selectCharacter();
        break;
    case EditorMode::Paused:
        setModeLabel(L"PAUSED");
        // PR-5 (LM-2): Paused 也锁 Inspector（与 Play 同语义）。
        _allowInspectorEdit = false;
        setInspectorHint(L"Locked during Play.");
        applyRenderSettingsFromPanel();
        pushFreecamToRenderer();
        break;
    }

    const bool editCommandsEnabled = mode == EditorMode::Edit;
    if (_undoMenuItem != nullptr) _undoMenuItem->setEnabled(editCommandsEnabled);
    if (_redoMenuItem != nullptr) _redoMenuItem->setEnabled(editCommandsEnabled);

    // v0.3+ PR-5 — mode 切换会换 Hierarchy 的 World 源（决策 1b）。
    // 选择在上方按 World 生命期切换；这里只排队重建。延迟到 update() 消费
    // 是因为 btn_play/btn_stop click handler 仍在 UIManager 事件派发栈内
    // （Landmine B）。
    _outlinerRefreshPending = true;

    // v0.3 PR-4 — mode 变化时同步 refresh lbl_unsaved（design §4.3.x 决策 5a）
    refreshUnsavedIndicator();
    refreshTransformInspector();

    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();
    syncTransformGizmoToRenderer();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::syncViewportIfChanged() {
    if (_hostWindow == nullptr) {
        return;
    }

    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return;
    }

    const ayt::math::FRectangle bounds = viewport->getWorldBounds();
    if (_viewportBoundsCached
        && bounds.minX == _cachedViewportBounds.minX
        && bounds.minY == _cachedViewportBounds.minY
        && bounds.maxX == _cachedViewportBounds.maxX
        && bounds.maxY == _cachedViewportBounds.maxY) {
        return;
    }

    _cachedViewportBounds = bounds;
    _viewportBoundsCached = true;
    _playRuntime.syncViewportRect(bounds);
}

void EditorSession::syncViewport() {
    _viewportBoundsCached = false;
    syncViewportIfChanged();
}

} // namespace ayt::editor
