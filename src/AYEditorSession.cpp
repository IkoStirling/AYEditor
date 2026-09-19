#include "AYEditor/EditorSession.h"

#if defined(_DEBUG) && defined(_MSC_VER)
#include "AYEditor/EditorHeapDebug.h"
#endif
#include "AYEditor/EditorProductPaths.h"
#include "AYEditor/EditorVisualStyle.h"
#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/EditorAssetDeleteAnalysis.h"
#include "AYEditor/EditorAssetImportQueue.h"
#include "AYEditor/EditorAssetTrash.h"
#include "AYEditor/EditorAssetOperations.h"
#include "AYEditor/EditorProjectAssetFactory.h"
#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorProjectRunner.h"
#include "AYEditor/EditorRecoveryStore.h"
#include "AYEditor/EditorProjectRuntimeValidator.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorComponentPolicy.h"
#include "AYEditor/EditorShortcutRegistry.h"
#include "AYEditor/EditorSceneCamera.h"
#include "AYEditor/EditorSceneViewWorkspace.h"
#include "AYEditor/EditorDslDocument.h"
#include "AYEditor/EditorDslExtension.h"
#include "AYEditor/EditorDockViewHost.h"
#include "AYEditor/EditorUiLayoutExtension.h"
#include "AYEditor/EditorUiLayoutDocument.h"
#include "AYEditor/EditorUiFlowExtension.h"
#include "AYEditor/EditorUiFlowDocument.h"
#include "AYEditor/EditorGameFlowExtension.h"
#include "AYEditor/EditorUiDesignerWorkflow.h"
#include "AYEditor/EditorWorkspace.h"
#include "AYEntity.h"
#include "AYUI/SplitterHandle.h"
#include "AYUI/Button.h"
#include "AYUI/CheckBox.h"
#include "AYUI/ComboBox.h"
#include "AYUI/ColorPicker.h"
#include "AYUI/Box.h"
#include "AYGameLoop.h"
#include "AYUI/MenuBar.h"
#include "AYUI/Menu.h"
#include "AYUI/MenuItem.h"
#include "AYUI/ModalDialog.h"
#include "AYUI/Panel.h"
#include "AYRenderer/RendererSubSystem.h"
#include "AYUI/Slider.h"
#include "AYUI/SvgIcon.h"
#include "AYUI/TextLabel.h"
#include "AYUI/TextInput.h"
#include "AYUI/TextArea.h"
#include "AYUI/Image.h"
#include "AYUI/Theme.h"
#include "AYUI/Tooltip.h"
#include "AYUI/TreeView.h"  // v0.3+ PR-5 Hierarchy panel (design §4.3.y)
#include "AYUI/ListView.h"
#include "AYUI/TileView.h"
#include "AYUI/UnicodeText.h"
#include "AYUI/Widget.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include <AYReflect.h>
#include "AudioEditorSession.h"
#include "EditorAssetPreviewCache.h"
#include "AYAudio/AudioSubSystem.h"
#include "AYUI/UIKeyCode.h"
#include "AYDevice/DeviceManager.h"
#include <AYApplication/GameFlowStandardActions.h>
#include <AYLocalization.h>

// v0.3 PR-4 — Editor 消费 host->scenes()（design §4.2.x + §4.3.x）
// AYScene 完整 include 因文档层需 SceneMode/Scene 完整类型；
// IEngineHost 走 host facade（v0.1.3 PR-6 ship）。
#include "AYScene.h"
#include "AYScene/SceneManager.h"
#include "AYScene/SceneMode.h"
#include "AYApplication/IEngineHost.h"
#include "AYApplication.h"  // currentEngineHost() / defaultEngineHost()

#include <AYEntity/components/AnimationComponent.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYEntity/components/OrthoCameraComponent.h>
#include <AYEntity/components/SpriteComponent.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYEntity/components/TilemapComponent.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYEntity/OrthoCameraSelection.h>
#include <AYMath/MathTransform.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/AssetPath.h>
#include <AYResource/VirtualAssetPath.h>
#include <AYResource/assetsDefs/IMesh.h>
#include <AYResource/assetsDefs/ITexture.h>
#include <AYResource/assetsDefs/ITilemap.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <limits>
#include <type_traits>
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
#include <vector>

namespace ayt::editor {

namespace {

bool editorUiTimingsEnabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("AY_EDITOR_UI_TIMINGS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

std::uint64_t editorUiElapsedMicroseconds(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now())
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::microseconds>(end - start).count());
}

// EditorAssetPreviewCache owns and releases this texture. Image normally owns
// anonymous handles, so keep the borrowed handle out of Image's release path
// while retaining its normal textured-quad rendering behavior.
class EditorBorrowedImage final : public ayt::ui::Image {
public:
    ~EditorBorrowedImage() override
    {
        _tex = {};
    }

    void setBorrowedTexture(const ayt::ui::ImageTextureHandle& texture)
    {
        _tex = texture;
        markDirty();
    }

    void clearBorrowedTexture()
    {
        if (_tex.handle == nullptr && _tex.name.empty()) return;
        _tex = {};
        markDirty();
    }
};

struct EditorTooltipBinding {
    ayt::ui::Tooltip* tooltip = nullptr;
    ayt::ui::Widget* target = nullptr;
    std::string key;
    std::wstring fallback;
    std::wstring suffix;
};

struct EditorMenuTextBinding {
    ayt::ui::MenuItem* item = nullptr;
    ayt::ui::MenuBar* menuBar = nullptr;
    std::size_t menuIndex = 0u;
    std::string key;
    std::wstring fallback;
};

std::unordered_map<const EditorSession*, std::vector<EditorTooltipBinding>>
    gEditorTooltips;
std::unordered_map<const EditorSession*, std::vector<EditorMenuTextBinding>>
    gEditorMenuTexts;

// Read-only project HUD preview layered over the native Scene View. It clips
// authored layouts to the viewport and deliberately yields pointer input to
// Scene picking and gizmos.
class PassiveSceneUiPreviewHost final : public ayt::ui::CompoundWidget {
public:
    ayt::ui::Widget* hitTest(
        const ayt::math::FVector2& /*worldPos*/) override {
        return nullptr;
    }

protected:
    void renderChildren(ayt::ui::IRenderBackend& renderer) override {
        renderer.pushClip(getWorldBounds());
        ayt::ui::CompoundWidget::renderChildren(renderer);
        renderer.popClip();
    }
};

// Inspector numeric editor: keeps an exact backing string while presenting a
// compact value in dense rows. A click restores the full value before
// TextInput places the caret; horizontal drag then uses TextInput's numeric
// scrub path. Hosts may temporarily redistribute the containing row on focus.
class InspectorNumberInput final : public ayt::ui::TextInput {
public:
    void setInspectorText(const std::wstring& precise, bool integral = false,
                          bool unsignedIntegral = false,
                          int significantDigits = 9) {
        _precise = precise;
        _integral = integral;
        _unsignedIntegral = integral && unsignedIntegral;
        _significantDigits = std::clamp(significantDigits, 1, 17);
        ayt::ui::TextInput::setText(hasFocus() ? _precise : compact(_precise));
    }

    void setInspectorValue(double value) {
        wchar_t buffer[64] = {};
        if (_integral) {
            std::swprintf(buffer, std::size(buffer), L"%.0f", value);
        } else {
            std::swprintf(buffer, std::size(buffer), L"%.*g",
                          _significantDigits, value);
        }
        setInspectorText(buffer, _integral, _unsignedIntegral,
                         _significantDigits);
    }

    const std::wstring& preciseText() const noexcept { return _precise; }

    void applyInspectorScrubDelta(double delta) {
        if (!std::isfinite(delta)) return;
        if (!_integral) {
            double base = 0.0;
            if (!parseFloating(_scrubOrigin, base)) return;
            setInspectorValue(base + delta);
            return;
        }

        const std::int64_t wholeDelta = roundedIntegralDelta(delta);
        if (_unsignedIntegral) {
            std::uint64_t base = 0;
            if (!parseUnsigned(_scrubOrigin, base)) return;
            std::uint64_t next = base;
            if (wholeDelta >= 0) {
                const auto amount = static_cast<std::uint64_t>(wholeDelta);
                next = amount > std::numeric_limits<std::uint64_t>::max() - base
                    ? std::numeric_limits<std::uint64_t>::max()
                    : base + amount;
            } else {
                const std::uint64_t amount = wholeDelta
                    == std::numeric_limits<std::int64_t>::lowest()
                    ? std::uint64_t{1} << 63u
                    : static_cast<std::uint64_t>(-wholeDelta);
                next = amount > base ? 0u : base - amount;
            }
            setInspectorText(std::to_wstring(next), true, true,
                             _significantDigits);
            return;
        }

        std::int64_t base = 0;
        if (!parseSigned(_scrubOrigin, base)) return;
        std::int64_t next = base;
        if (wholeDelta > 0
            && base > std::numeric_limits<std::int64_t>::max() - wholeDelta) {
            next = std::numeric_limits<std::int64_t>::max();
        } else if (wholeDelta < 0
                   && base < std::numeric_limits<std::int64_t>::lowest()
                               - wholeDelta) {
            next = std::numeric_limits<std::int64_t>::lowest();
        } else {
            next = base + wholeDelta;
        }
        setInspectorText(std::to_wstring(next), true, false,
                         _significantDigits);
    }

    void setFocusLayoutCallback(std::function<void(bool)> callback) {
        _focusLayout = std::move(callback);
    }

protected:
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override {
        _scrubOrigin = hasFocus() ? getText() : _precise;
        return ayt::ui::TextInput::onMouseButtonDown(event);
    }

    void onFocusGained() override {
        ayt::ui::TextInput::setText(_precise);
        ayt::ui::TextInput::onFocusGained();
        if (_focusLayout) _focusLayout(true);
    }

    void onFocusLost() override {
        // TextInput fires the host commit callback here while the precise,
        // user-edited string is still installed.
        ayt::ui::TextInput::onFocusLost();
        _precise = getText();
        ayt::ui::TextInput::setText(compact(_precise));
        if (_focusLayout) _focusLayout(false);
    }

private:
    static bool parseFloating(const std::wstring& source, double& value) {
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(source.c_str(), &end);
        if (end == source.c_str() || end == nullptr || *end != L'\0'
            || !std::isfinite(parsed)) {
            return false;
        }
        value = parsed;
        return true;
    }

    static bool parseSigned(const std::wstring& source, std::int64_t& value) {
        errno = 0;
        wchar_t* end = nullptr;
        const long long parsed = std::wcstoll(source.c_str(), &end, 10);
        if (errno == ERANGE || end == source.c_str() || end == nullptr
            || *end != L'\0') {
            return false;
        }
        value = static_cast<std::int64_t>(parsed);
        return true;
    }

    static bool parseUnsigned(const std::wstring& source,
                              std::uint64_t& value) {
        if (!source.empty() && source.front() == L'-') return false;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed =
            std::wcstoull(source.c_str(), &end, 10);
        if (errno == ERANGE || end == source.c_str() || end == nullptr
            || *end != L'\0') {
            return false;
        }
        value = static_cast<std::uint64_t>(parsed);
        return true;
    }

    static std::int64_t roundedIntegralDelta(double delta) {
        if (delta >= static_cast<double>(
                         std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (delta <= static_cast<double>(
                         std::numeric_limits<std::int64_t>::lowest())) {
            return std::numeric_limits<std::int64_t>::lowest();
        }
        return static_cast<std::int64_t>(std::llround(delta));
    }

    static std::wstring compact(const std::wstring& source) {
        if (source.empty()) return source;
        wchar_t* end = nullptr;
        const double value = std::wcstod(source.c_str(), &end);
        if (end == source.c_str() || (end != nullptr && *end != L'\0')
            || !std::isfinite(value)) {
            return source.size() <= 6u ? source : source.substr(0u, 6u);
        }
        const double normalized = std::fabs(value) < 5.0e-10 ? 0.0 : value;
        if (_isIntegralText(source)) {
            wchar_t integerBuffer[64] = {};
            std::swprintf(integerBuffer, std::size(integerBuffer), L"%.0f",
                          normalized);
            const std::wstring result(integerBuffer);
            if (result.size() <= 6u) return result;
        }
        for (int decimals = 3; decimals >= 0; --decimals) {
            wchar_t buffer[64] = {};
            std::swprintf(buffer, std::size(buffer), L"%.*f", decimals,
                          normalized);
            std::wstring result(buffer);
            while (result.find(L'.') != std::wstring::npos
                   && !result.empty() && result.back() == L'0') {
                result.pop_back();
            }
            if (!result.empty() && result.back() == L'.') result.pop_back();
            if (result.size() <= 6u) return result;
        }
        wchar_t scientific[64] = {};
        std::swprintf(scientific, std::size(scientific), L"%.1g", normalized);
        return scientific;
    }

    static bool _isIntegralText(const std::wstring& value) {
        return value.find(L'.') == std::wstring::npos
            && value.find(L'e') == std::wstring::npos
            && value.find(L'E') == std::wstring::npos;
    }

    std::wstring _precise;
    std::wstring _scrubOrigin;
    bool _integral = false;
    bool _unsignedIntegral = false;
    int _significantDigits = 9;
    std::function<void(bool)> _focusLayout;
};

void setEditorMenuToggleIcon(ayt::ui::MenuItem* item,
                             const std::string& engineAssetsRoot,
                             bool checked)
{
    if (item == nullptr || engineAssetsRoot.empty()) return;
    const std::filesystem::path path =
        std::filesystem::path(engineAssetsRoot) / "Icons" / "Tabler"
        / "outline" / (checked ? "check.svg" : "x.svg");
    std::string error;
    item->setLeadingIconDocument(
        ayt::ui::SvgDocument::loadFromFile(path, &error));
    item->setLeadingIconSize(13.0f);
    item->setLeadingIconColor(checked
        ? ayt::math::FVector4(0.35f, 0.72f, 1.0f, 1.0f)
        : ayt::math::FVector4(0.50f, 0.53f, 0.58f, 0.85f));
}

void attachEditorTooltip(const EditorSession* session, ayt::ui::Widget* target,
                         std::string key, std::wstring fallback,
                         const std::wstring& text,
                         std::wstring suffix = {})
{
    if (session == nullptr || target == nullptr || text.empty()) return;
    auto& bindings = gEditorTooltips[session];
    for (EditorTooltipBinding& binding : bindings) {
        if (binding.target != target || binding.tooltip == nullptr) continue;
        binding.key = std::move(key);
        binding.fallback = std::move(fallback);
        binding.suffix = std::move(suffix);
        binding.tooltip->setText(text);
        return;
    }
    if (auto* tooltip = ayt::ui::Tooltip::attachTo(target)) {
        tooltip->setText(text);
        bindings.push_back({tooltip, target, std::move(key),
                            std::move(fallback), std::move(suffix)});
    }
}

void clearEditorTooltips(const EditorSession* session)
{
    gEditorMenuTexts.erase(session);
    const auto found = gEditorTooltips.find(session);
    if (found == gEditorTooltips.end()) return;
    for (const EditorTooltipBinding& binding : found->second) {
        ayt::ui::Tooltip* tooltip = binding.tooltip;
        if (tooltip == nullptr) continue;
        tooltip->detach();
        ayt::ui::destroyWidgetTree(tooltip);
    }
    gEditorTooltips.erase(found);
}

class EditorSessionHostServices final : public IEditorHostServices {
public:
    EditorSessionHostServices(
        EditorWorkspace& workspace,
        ayt::ui::UIManager& ui,
        std::string projectRoot,
        EditorAssetPreviewCache* imageCache,
        void* ownerWindow,
        std::function<void()> repaint,
        std::function<void(const std::wstring&)> setStatus,
        std::function<std::wstring(std::string_view, std::wstring_view)>
            localize,
        std::function<std::string()> currentLanguage)
        : _workspace(workspace), _ui(ui),
          _projectRoot(std::move(projectRoot)),
          _imageCache(imageCache), _ownerWindow(ownerWindow),
          _repaint(std::move(repaint)), _setStatus(std::move(setStatus)),
          _localize(std::move(localize)),
          _currentLanguage(std::move(currentLanguage)) {}

    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override {
        return _projectRoot;
    }
    ayt::ui::UIManager* uiManager() noexcept override {
        return &_ui;
    }
    std::string chooseImageFile() override {
        return ImportDialog::showOpenImageFileDialog(_ownerWindow);
    }
    EditorAuthoringImage loadAuthoringImage(
        const std::string& path, std::string* error = nullptr) override {
        if (_imageCache == nullptr) {
            if (error != nullptr) *error = "The image cache is unavailable.";
            return {};
        }
        return _imageCache->loadAuthoringImage(path, error);
    }
    void requestRepaint() override {
        if (_repaint) _repaint();
    }
    void setStatusText(const std::wstring& text) override {
        if (_setStatus) _setStatus(text);
    }
    std::wstring localizedText(
        std::string_view key, std::wstring_view fallback) const override {
        return _localize ? _localize(key, fallback) : std::wstring(fallback);
    }
    std::string currentLanguage() const override {
        return _currentLanguage ? _currentLanguage() : std::string{};
    }

private:
    EditorWorkspace& _workspace;
    ayt::ui::UIManager& _ui;
    std::string _projectRoot;
    EditorAssetPreviewCache* _imageCache = nullptr;
    void* _ownerWindow = nullptr;
    std::function<void()> _repaint;
    std::function<void(const std::wstring&)> _setStatus;
    std::function<std::wstring(std::string_view, std::wstring_view)> _localize;
    std::function<std::string()> _currentLanguage;
};

// DeviceInputBridge converts wheel notches to AYUI logical pixels before it
// reaches EditorSession. Keep the inverse conversion explicit here: treating
// the default 40 pixels as 40 notches made one physical notch hit Freecam's
// eight-notch safety clamp and produced the touchpad sensitivity spike.
constexpr float kWheelLogicalPixelsPerNotch = 40.0f;
constexpr float kViewportLookDragSlopPixels = 8.0f;
constexpr float kTwoDGizmoSizePixels = 72.0f;
constexpr uint16_t kTwoDGizmoDisabledHandleMask =
    EditorTransformGizmo::handleBit(EditorGizmoHandle::AxisZ)
  | EditorTransformGizmo::handleBit(EditorGizmoHandle::PlaneYZ)
  | EditorTransformGizmo::handleBit(EditorGizmoHandle::PlaneZX)
  | EditorTransformGizmo::handleBit(EditorGizmoHandle::RingX)
  | EditorTransformGizmo::handleBit(EditorGizmoHandle::RingY)
  | EditorTransformGizmo::handleBit(EditorGizmoHandle::ScaleZ);
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

bool endsWithInsensitive(const std::string& value, const char* suffix)
{
    const std::size_t suffixLength = std::strlen(suffix);
    if (value.size() < suffixLength) return false;
    const std::size_t offset = value.size() - suffixLength;
    for (std::size_t index = 0; index < suffixLength; ++index) {
        const unsigned char lhs = static_cast<unsigned char>(value[offset + index]);
        const unsigned char rhs = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

std::string tilemapRuntimeReference(const EditorAssetDatabase& database,
                                    const EditorAssetRecord& record)
{
    if (!endsWithInsensitive(record.name, ".aytilemap.json")) {
        return database.portableAssetPath(record);
    }
    std::filesystem::path stem(record.name);
    stem = stem.stem().stem();
    return ayt::resource::makeTilemapVirtualPath(stem.string());
}

std::string editorRuntimeAssetPath(const EditorAssetDatabase& database,
                                   const std::string& reference)
{
    if (reference.empty()) return {};
    std::filesystem::path path(reference);
    if (path.is_absolute()) return path.lexically_normal().string();
    for (const std::string* root : {&database.sourceRoot(),
                                    &database.derivedRoot()}) {
        std::filesystem::path candidate =
            std::filesystem::path(*root) / path;
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
            return candidate.lexically_normal().string();
        }
    }
    return (std::filesystem::path(database.sourceRoot()) / path)
        .lexically_normal().string();
}

struct Editor2DSelectionShape {
    ayt::math::FVector2 localMin{};
    ayt::math::FVector2 localMax{};
    bool ignoreTransformScale = false;
    bool cameraFrame = false;
    int priority = 0;
    int layer = 0;
    int sortingKey = 0;
};

bool editor2DSelectionShape(ayt::entity::Entity& entity,
                            const EditorAssetDatabase& database,
                            float viewportAspect,
                            Editor2DSelectionShape& out)
{
    if (auto* sprite = entity.getComponent<ayt::entity::SpriteComponent>();
        sprite != nullptr && sprite->visible) {
        out.localMin = {-0.5f, -0.5f};
        out.localMax = {0.5f, 0.5f};
        out.priority = 2;
        out.layer = sprite->layer;
        out.sortingKey = sprite->sortingKey;
        return true;
    }
    if (auto* tilemap = entity.getComponent<ayt::entity::TilemapComponent>();
        tilemap != nullptr && tilemap->visible) {
        float width = 32.0f;
        float height = 32.0f;
        if (!tilemap->tilemapPath.empty()) {
            try {
                const auto resource = ayt::resource::ResourceManager::instance()
                    .load<ayt::resource::ITilemap>(editorRuntimeAssetPath(
                        database, tilemap->tilemapPath));
                if (resource != nullptr) {
                    width = static_cast<float>(resource->getCols())
                          * static_cast<float>(resource->getTileWidth());
                    height = static_cast<float>(resource->getRows())
                           * static_cast<float>(resource->getTileHeight());
                }
            } catch (...) {
            }
        }
        out.localMin = {0.0f, 0.0f};
        out.localMax = {std::max(width, 1.0f), std::max(height, 1.0f)};
        out.priority = 2;
        out.layer = tilemap->layer;
        out.sortingKey = tilemap->sortingKey;
        return true;
    }
    if (auto* camera = entity.getComponent<ayt::entity::OrthoCameraComponent>()) {
        const ayt::math::FVector2 half = camera->visibleHalfExtents(viewportAspect);
        out.localMin = {-half.x, -half.y};
        out.localMax = { half.x,  half.y};
        out.ignoreTransformScale = true;
        out.cameraFrame = true;
        out.priority = 1;
        return true;
    }
    return false;
}

ayt::math::Float4x4 editor2DShapeMatrix(
    const ayt::entity::Transform& transform,
    const Editor2DSelectionShape& shape)
{
    const ayt::math::FVector3 scale = shape.ignoreTransformScale
        ? ayt::math::FVector3(1.0f, 1.0f, 1.0f) : transform.scale;
    return ayt::math::Transform::getMatrix(
        transform.position, transform.rotation, scale);
}

bool pointHitsEditor2DShape(const Editor2DSelectionShape& shape,
                            const ayt::math::FVector3& localPoint,
                            float worldTolerance)
{
    if (!shape.cameraFrame) {
        return localPoint.x >= shape.localMin.x
            && localPoint.x <= shape.localMax.x
            && localPoint.y >= shape.localMin.y
            && localPoint.y <= shape.localMax.y;
    }
    const bool withinX = localPoint.x >= shape.localMin.x - worldTolerance
        && localPoint.x <= shape.localMax.x + worldTolerance;
    const bool withinY = localPoint.y >= shape.localMin.y - worldTolerance
        && localPoint.y <= shape.localMax.y + worldTolerance;
    if (!withinX || !withinY) return false;
    const float edgeDistance = std::min({
        std::fabs(localPoint.x - shape.localMin.x),
        std::fabs(localPoint.x - shape.localMax.x),
        std::fabs(localPoint.y - shape.localMin.y),
        std::fabs(localPoint.y - shape.localMax.y)});
    const bool nearCenter = std::fabs(localPoint.x) <= worldTolerance
        && std::fabs(localPoint.y) <= worldTolerance;
    return edgeDistance <= worldTolerance || nearCenter;
}

bool isEditor2DResourceField(const std::string& componentType,
                             const std::string& fieldName)
{
    if (componentType == "SpriteComponent") {
        return fieldName == "texturePath"
            || fieldName == "normalTexturePath"
            || fieldName == "roughnessTexturePath"
            || fieldName == "emissiveTexturePath";
    }
    if (componentType == "TilemapComponent") {
        return fieldName == "tilemapPath"
            || fieldName == "atlasTexturePath"
            || fieldName == "normalTexturePath"
            || fieldName == "roughnessTexturePath"
            || fieldName == "emissiveTexturePath"
            || fieldName == "atlasPath";
    }
    return false;
}

std::vector<std::wstring> editor2DEnumItems(
    const std::string& componentType, const std::string& fieldName)
{
    if ((componentType == "SpriteComponent"
         || componentType == "TilemapComponent")
        && fieldName == "renderDomain") {
        return {L"Camera Overlay", L"World Lit"};
    }
    if (componentType == "SpriteComponent" && fieldName == "flip") {
        return {L"None", L"Horizontal", L"Vertical", L"Both"};
    }
    if (componentType == "TilemapComponent"
        && fieldName == "samplingQuality") {
        return {L"Nearest", L"Linear", L"4-tap", L"9-tap"};
    }
    if (componentType == "OrthoCameraComponent"
        && fieldName == "aspectPolicy") {
        return {L"Expand", L"Fit", L"Fill"};
    }
    return {};
}

std::string resolveLayoutEditorChromePath(
    const std::string& engineAssetsRoot) {
    const std::string path = (std::filesystem::path(engineAssetsRoot)
        / "AYUI" / "ui" / "layout_editor.ui.json").string();
    return path;
}

std::string resolveUiFlowEditorChromePath(
    const std::string& engineAssetsRoot)
{
    return (std::filesystem::path(engineAssetsRoot)
        / "AYEditor" / "ui" / "ui_flow_editor.ui.json").string();
}

std::string resolveProjectAssetRoot(const std::string& projectRoot)
{
    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(projectRoot, &error);
    const std::string relative = descriptor && !descriptor.assetRoot.empty()
        ? descriptor.assetRoot : "Assets";
    return (std::filesystem::path(projectRoot) / relative).string();
}

std::vector<ayt::ui::LayoutProjectRefactorKind> uiProjectRefactorKinds()
{
    using Kind = ayt::ui::LayoutProjectRefactorKind;
    return {
        {"widget-id", L"Widget ID", Kind::Seed::SelectedWidgetId},
        {"widget-handler", L"Widget Handler", Kind::Seed::None},
        {"flow-signal", L"Flow Signal", Kind::Seed::None},
        {"layout-reference", L"Layout Reference", Kind::Seed::DocumentPath},
    };
}

std::vector<ayt::ui::LayoutTextureResource> enumerateUiTextureResources(
    const std::string& engineAssetsRoot, const std::string& projectRoot) {
    namespace fs = std::filesystem;
    std::vector<ayt::ui::LayoutTextureResource> resources;
    std::unordered_map<std::string, bool> seen;
    auto scan = [&](const fs::path& root, const std::string& keyPrefix,
                    const std::wstring& displayPrefix) {
        std::error_code error;
        if (!fs::is_directory(root, error)) return;
        for (fs::recursive_directory_iterator it(
                 root, fs::directory_options::skip_permission_denied, error), end;
             it != end && resources.size() < 4096u; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (!it->is_regular_file(error)) continue;
            std::string extension = it->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            if (extension != ".png" && extension != ".jpg" &&
                extension != ".jpeg" && extension != ".bmp" &&
                extension != ".tga") continue;
            const fs::path relative = fs::relative(it->path(), root, error);
            if (error) {
                error.clear();
                continue;
            }
            const std::string key = keyPrefix + relative.generic_string();
            if (!seen.emplace(key, true).second) continue;
            ayt::ui::LayoutTextureResource resource;
            resource.key = key;
            resource.displayName = displayPrefix + relative.generic_wstring();
            resource.previewPath = fs::absolute(it->path(), error)
                .lexically_normal().string();
            error.clear();
            resource.detail = it->path().extension().wstring();
            resources.push_back(std::move(resource));
        }
    };
    if (!projectRoot.empty()) {
        scan(fs::u8path(projectRoot) / "Assets", "Assets/", L"Project / ");
    }
    if (!engineAssetsRoot.empty()) {
        scan(fs::u8path(engineAssetsRoot), "EngineAssets/", L"Engine / ");
    }
    return resources;
}

std::string resolveAudioEditorChromePath(
    const std::string& engineAssetsRoot) {
    const std::string path = (std::filesystem::path(engineAssetsRoot)
        / "AYAudio" / "ui" / "audio_editor.ui.json").string();
    return path;
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

std::string showUiJsonOpenDialog(HWND owner,
                                 const std::string& initialDirectory = {}) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
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

std::string showAssetReferenceDialog(HWND owner, const std::string& projectRoot)
{
    char path[MAX_PATH] = {};
    const std::string initialDirectory =
        (std::filesystem::path(projectRoot) / "Assets").string();
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initialDirectory.c_str();
    ofn.lpstrFilter =
        "Project assets\0*.aymesh;*.aymat;*.ayanim;*.ayskel;*.aytex;*.aytilemap;*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.wav;*.mp3;*.ogg;*.json\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return ::GetOpenFileNameA(&ofn) ? std::string(path) : std::string{};
}

std::string showUiJsonSaveDialog(HWND owner,
                                 const std::string& initialDirectory = {}) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
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

std::string showUiFlowOpenDialog(HWND owner,
                                 const std::string& initialDirectory = {})
{
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
    ofn.lpstrFilter =
        "AYUI Flow (*.uiflow.json)\0*.uiflow.json\0"
        "JSON (*.json)\0*.json\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "uiflow.json";
    return ::GetOpenFileNameA(&ofn) ? std::string(path) : std::string{};
}

std::string showUiFlowSaveDialog(HWND owner,
                                 const std::string& initialDirectory = {})
{
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
    ofn.lpstrFilter =
        "AYUI Flow (*.uiflow.json)\0*.uiflow.json\0"
        "JSON (*.json)\0*.json\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "uiflow.json";
    return ::GetSaveFileNameA(&ofn) ? std::string(path) : std::string{};
}

std::string showUiTextureOpenDialog(HWND owner,
                                    const std::string& initialDirectory = {}) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
    ofn.lpstrFilter =
        "Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
        "All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!::GetOpenFileNameA(&ofn)) return {};
    return std::string(path);
}

std::string showSceneOpenDialog(HWND owner,
                                const std::string& initialDirectory = {}) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
    ofn.lpstrFilter = "AY Scene (*.ayscene)\0*.ayscene\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "ayscene";
    return ::GetOpenFileNameA(&ofn) ? std::string(path) : std::string{};
}

std::string showSceneSaveDialog(HWND owner,
                                const std::string& initialDirectory = {}) {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    if (!initialDirectory.empty()) ofn.lpstrInitialDir = initialDirectory.c_str();
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

std::wstring formatPreciseFloat(float value) {
    wchar_t buffer[48] = {};
    std::swprintf(buffer, std::size(buffer), L"%.9g",
                  static_cast<double>(value));
    return buffer;
}

std::wstring formatPreciseDouble(double value) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), L"%.17g", value);
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

bool parseDouble(const std::wstring& text, double& value)
{
    const wchar_t* begin = text.c_str();
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(begin, &end);
    while (end != nullptr && *end == L' ') ++end;
    if (begin == end || end == nullptr || *end != L'\0'
        || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

template<typename T>
bool isReflectedType(const ayt::reflect::ITypeInfo* type)
{
    return type != nullptr && type->getId() == typeid(T).hash_code();
}

template<typename T>
bool assignIntegralValue(void* address, double parsed)
{
    auto* target = static_cast<T*>(address);
    T next{};
    if (parsed <= static_cast<double>(std::numeric_limits<T>::lowest())) {
        next = std::numeric_limits<T>::lowest();
    } else if (parsed >= static_cast<double>(std::numeric_limits<T>::max())) {
        next = std::numeric_limits<T>::max();
    } else {
        next = static_cast<T>(parsed);
    }
    if (*target == next) return false;
    *target = next;
    return true;
}

template<typename T>
bool assignIntegralText(void* address, const std::wstring& text)
{
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    const wchar_t* begin = text.c_str();
    while (*begin == L' ') ++begin;
    if (*begin == L'\0') return false;

    wchar_t* end = nullptr;
    T next{};
    errno = 0;
    if constexpr (std::is_signed_v<T>) {
        const long long parsed = std::wcstoll(begin, &end, 10);
        while (end != nullptr && *end == L' ') ++end;
        if (end == begin || end == nullptr || *end != L'\0') return false;
        if (errno == ERANGE
            || parsed <= static_cast<long long>(
                std::numeric_limits<T>::lowest())) {
            next = std::numeric_limits<T>::lowest();
        } else if (parsed >= static_cast<long long>(
                       std::numeric_limits<T>::max())) {
            next = std::numeric_limits<T>::max();
        } else {
            next = static_cast<T>(parsed);
        }
    } else {
        const bool negative = *begin == L'-';
        const unsigned long long parsed = std::wcstoull(begin, &end, 10);
        while (end != nullptr && *end == L' ') ++end;
        if (end == begin || end == nullptr || *end != L'\0') return false;
        if (negative) {
            next = 0;
        } else if (errno == ERANGE
                   || parsed >= static_cast<unsigned long long>(
                       std::numeric_limits<T>::max())) {
            next = std::numeric_limits<T>::max();
        } else {
            next = static_cast<T>(parsed);
        }
    }
    auto* target = static_cast<T*>(address);
    if (*target == next) return false;
    *target = next;
    return true;
}

bool isDescendantOf(const ayt::ui::Widget* widget,
                    const ayt::ui::Widget* ancestor)
{
    for (auto* current = widget; current != nullptr;
         current = current->getParent()) {
        if (current == ancestor) return true;
    }
    return false;
}

ayt::ui::Widget* findDescendantById(ayt::ui::Widget* root,
                                    const std::string& id)
{
    if (root == nullptr) return nullptr;
    if (root->getId() == id) return root;
    for (ayt::ui::Widget* child : root->getChildren()) {
        if (auto* found = findDescendantById(child, id)) return found;
    }
    return nullptr;
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

std::string systemLanguageTag()
{
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (::GetUserDefaultLocaleName(
            localeName, LOCALE_NAME_MAX_LENGTH) <= 0) {
        return "en-US";
    }
    return wideToUtf8(localeName);
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

std::wstring assetModifiedText(std::int64_t ticks)
{
    if (ticks == 0) return L"unknown";
    using FileTime = std::filesystem::file_time_type;
    const FileTime fileTime{FileTime::duration(ticks)};
    const auto systemTime = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
            fileTime - FileTime::clock::now()
            + std::chrono::system_clock::now());
    const std::time_t value = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &value) != 0) return L"unknown";
#else
    if (localtime_r(&value, &local) == nullptr) return L"unknown";
#endif
    wchar_t buffer[32]{};
    if (std::wcsftime(buffer, sizeof(buffer) / sizeof(buffer[0]),
                      L"%Y-%m-%d %H:%M", &local)
        == 0) {
        return L"unknown";
    }
    return buffer;
}

} // namespace

EditorSession::EditorSession()
    : _gameView(ayt::game::GameLoop::instance(), _playRuntime) {
    _worldContext.setFallbackWorld(&ayt::entity::World::instance());
    _playRuntime.setWorldContext(&_worldContext);
    _workspace = std::make_unique<EditorWorkspace>();
    std::string error;
    if (!registerEditorDslExtension(_workspace->registry(), &error)) {
        std::fprintf(stderr,
            "[EditorSession] DSL workspace registration failed: %s\n",
            error.c_str());
    }
    EditorUiLayoutExtensionConfig layoutConfig;
    layoutConfig.chromePath = [this]() {
        return resolveLayoutEditorChromePath(_engineAssetsRoot);
    };
    layoutConfig.openPathPicker = [this]() {
        return showUiJsonOpenDialog(static_cast<HWND>(_hostWindow),
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    layoutConfig.savePathPicker = [this]() {
        return showUiJsonSaveDialog(static_cast<HWND>(_hostWindow),
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    layoutConfig.texturePathPicker = [this]() {
        return showUiTextureOpenDialog(static_cast<HWND>(_hostWindow),
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "textures").string());
    };
    layoutConfig.themePathPicker = [this]() {
        return showUiJsonOpenDialog(static_cast<HWND>(_hostWindow),
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui" / "themes").string());
    };
    layoutConfig.textureResourceProvider = [this]() {
        return enumerateUiTextureResources(
            _engineAssetsRoot, _assetDatabase.projectRoot());
    };
    layoutConfig.externalComponentLibraryPath = [this]() {
        return (std::filesystem::path(resolveProjectAssetRoot(
                    _assetDatabase.projectRoot()))
                / "ui" / "project.ayuicomponents.json").string();
    };
    layoutConfig.openOwningFlowAction = [this](
        const std::string& layoutPath, std::string& message) {
        return openOwningFlowForLayout(layoutPath, message);
    };
    layoutConfig.completeFlowSignalsAction = [this](
        const std::string& layoutPath, std::string& message) {
        return completeFlowSignalsForLayout(layoutPath, message);
    };
    layoutConfig.projectRefactorKinds = uiProjectRefactorKinds();
    layoutConfig.projectRefactorAction = [this](
        const std::string& layoutPath, const std::string& kind,
        const std::string& oldValue, const std::string& newValue, bool apply) {
        return refactorUiProjectReferences(
            layoutPath, kind, oldValue, newValue, apply);
    };
    error.clear();
    if (!registerEditorUiLayoutExtension(
            _workspace->registry(), std::move(layoutConfig), &error)) {
        std::fprintf(stderr,
            "[EditorSession] UI Layout workspace registration failed: %s\n",
            error.c_str());
    }
    EditorUiFlowExtensionConfig flowConfig;
    flowConfig.assetRoot = resolveProjectAssetRoot(
        _assetDatabase.projectRoot());
    flowConfig.chromePath = [this]() {
        return resolveUiFlowEditorChromePath(_engineAssetsRoot);
    };
    flowConfig.openPathPicker = [this]() {
        return showUiFlowOpenDialog(static_cast<HWND>(_hostWindow),
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    flowConfig.savePathPicker = [this]() {
        return showUiFlowSaveDialog(static_cast<HWND>(_hostWindow),
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    flowConfig.openLayoutForScreen = [this](
        const std::string& layoutAsset, std::string& message) {
        return openLayoutForFlowScreen(layoutAsset, message);
    };
    error.clear();
    if (!registerEditorUiFlowExtension(
            _workspace->registry(), std::move(flowConfig), &error)) {
        std::fprintf(stderr,
            "[EditorSession] UI Flow workspace registration failed: %s\n",
            error.c_str());
    }
    EditorGameFlowExtensionConfig gameFlowConfig;
    gameFlowConfig.configureRegistry = [](
        ayt::app::GameFlowActionRegistry& registry) {
        std::string registrationError;
        if (!ayt::app::registerGameFlowWorldActionType(
                registry, &registrationError)) {
            std::fprintf(stderr,
                "[EditorSession] GameFlow World metadata registration "
                "failed: %s\n", registrationError.c_str());
        }
        registrationError.clear();
        if (!ayt::app::registerGameFlowUIActionTypes(
                registry, &registrationError)) {
            std::fprintf(stderr,
                "[EditorSession] GameFlow UI metadata registration "
                "failed: %s\n", registrationError.c_str());
        }
    };
    error.clear();
    if (!registerEditorGameFlowExtension(
            _workspace->registry(), std::move(gameFlowConfig), &error)) {
        std::fprintf(stderr,
            "[EditorSession] GameFlow workspace registration failed: %s\n",
            error.c_str());
    }
    error.clear();
    EditorBuiltInExtensionConfig builtIns;
    builtIns.openAudioMixer = [this]() { openAudioEditorWindow(); };
    if (!registerEditorBuiltInExtensions(
            _workspace->registry(), std::move(builtIns), &error)) {
        std::fprintf(stderr,
            "[EditorSession] built-in extension registration failed: %s\n",
            error.c_str());
    }
}

EditorSession::~EditorSession() {
    shutdown();
}

EditorWorkspace& EditorSession::workspace() noexcept
{
    return *_workspace;
}

const EditorWorkspace& EditorSession::workspace() const noexcept
{
    return *_workspace;
}

std::size_t EditorSession::openDslDocumentCount() const noexcept
{
    return _dockViewHost != nullptr
        ? _dockViewHost->count(kEditorDslExtensionId) : 0u;
}

std::size_t EditorSession::openUiLayoutDocumentCount() const noexcept
{
    std::size_t count = 0;
    if (_workspace == nullptr) return count;
    for (const EditorDocumentRecord& record :
         _workspace->documents().records()) {
        if (record.editorId == kEditorUiLayoutExtensionId) ++count;
    }
    return count;
}

std::size_t EditorSession::openUiFlowDocumentCount() const noexcept
{
    std::size_t count = 0;
    if (_workspace == nullptr) return count;
    for (const EditorDocumentRecord& record : _workspace->documents().records()) {
        if (record.editorId == kEditorUiFlowExtensionId) ++count;
    }
    return count;
}

std::size_t EditorSession::openGameFlowDocumentCount() const noexcept
{
    return _dockViewHost != nullptr
        ? _dockViewHost->count(kEditorGameFlowExtensionId) : 0u;
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
    EditorComponentPolicyRegistry::instance().installDefaults();
    _devices = desc.deviceManager;
    _hostFocused = _devices != nullptr && _devices->window().isFocused();
    _layoutPath = desc.layoutPath;
    _engineAssetsRoot = desc.engineAssetsRoot;
    _projectRoot = desc.projectRoot;
    _assetTrash = std::make_unique<EditorAssetTrash>(desc.projectRoot);
    _assetOperations = std::make_unique<EditorAssetOperations>(desc.projectRoot);
    _assetImportQueue = std::make_unique<EditorAssetImportQueue>();
    const EditorProjectStartupSceneResolution projectStartup =
        resolveEditorProjectStartupScene(desc.projectRoot);
    std::string sceneViewWorkspaceError;
    if (!_sceneViewWorkspace.open(desc.projectRoot, &sceneViewWorkspaceError)) {
        std::fprintf(stderr, "[EditorSceneView] workspace ignored: %s\n",
                     sceneViewWorkspaceError.c_str());
    }
    if (projectStartup.projectDescriptorPresent) {
        std::string descriptorError;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(desc.projectRoot, &descriptorError);
        if (descriptor) {
            _projectDefaultSceneView = descriptor.defaultSceneView;
            _projectEngineProfile = descriptor.engineProfile;
        }
    }
    _assetImportProgressPercent = -1;
    _recoveryStore = std::make_unique<EditorRecoveryStore>(desc.projectRoot);
    std::string recoveryError;
    if (!_recoveryStore->beginSession(&recoveryError)) {
        std::fprintf(stderr, "[EditorSession] recovery unavailable: %s\n",
                     recoveryError.c_str());
    }
    if (_engineAssetsRoot.empty()) {
        _engineAssetsRoot =
            EditorProductPaths::detect().engineAssetsRoot.string();
    }
    auto& shortcuts = EditorShortcutRegistry::instance();
    shortcuts.resetToDefaults();
    std::string shortcutError;
    const std::filesystem::path engineShortcutPath =
        std::filesystem::path(_engineAssetsRoot) / "AYEditor" / "config"
        / "editor_shortcuts.json";
    if (!shortcuts.loadOverrides(engineShortcutPath.string(), &shortcutError)) {
        std::fprintf(stderr, "[EditorSession] shortcut defaults: %s\n",
                     shortcutError.c_str());
    }
    shortcutError.clear();
    const std::filesystem::path projectShortcutPath =
        std::filesystem::path(desc.projectRoot) / ".ayeditor"
        / "editor_shortcuts.json";
    if (!shortcuts.loadOverrides(projectShortcutPath.string(), &shortcutError)) {
        std::fprintf(stderr, "[EditorSession] shortcut overrides: %s\n",
                     shortcutError.c_str());
    }
    _preferences = desc.preferences;
    _localization =
        std::make_unique<ayt::localization::Localization>();
    _localization->loadAllFromDirectory(
        (std::filesystem::path(_engineAssetsRoot) / "AYEditor"
         / "Localization").string());
    _localization->setFallbackLanguage("en-US");
    _localization->setLanguage(_preferences.language == "system"
        ? systemLanguageTag() : _preferences.language);
    if (desc.createAssetPreviewTexture
        && desc.releaseAssetPreviewTexture) {
        _assetPreviewCache = std::make_unique<EditorAssetPreviewCache>(
            desc.createAssetPreviewTexture,
            desc.releaseAssetPreviewTexture);
    }
    _preferences.viewportOrientationAxisVisible =
        desc.viewportOrientationAxisVisible;
    _viewportOrientationAxisVisible =
        _preferences.viewportOrientationAxisVisible;
    _onViewportOrientationAxisVisibilityChanged =
        desc.onViewportOrientationAxisVisibilityChanged;
    _onPreferencesChanged = desc.onPreferencesChanged;
    _playRuntime.setHostWindow(static_cast<HWND>(_hostWindow));
    _playRuntime.setEngineAssetsRoot(_engineAssetsRoot);
    _playRuntime.setProjectAssetRoot(projectStartup.assetRootPath);
    // ED-02: forward the imported character (if any) to the
    // Play-runtime. Empty / invalid = cube fallback at startPlay.
    _playRuntime.setImportedCharacter(desc.importedCharacter);
    _playRuntime.setEditorTestSceneEnabled(
        desc.editorTestSceneEnabled
        && !projectStartup.projectDescriptorPresent);
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
    _ui.loader().setTextResolver(
        [this](std::string_view key, std::wstring_view fallback) {
            return ayt::ui::decodeUtf8Text(_localization->get(
                std::string(key), wideToUtf8(std::wstring(fallback))));
        });
    installEditorTheme(_preferences.themeName);
    _ui.setUiScale(std::clamp(_preferences.uiScale, 0.75f, 1.25f));
    AY_EDITOR_TRACE("initialize: ui backend set");
    reportStartup(0.12f, L"Loading editor layout...");
    _gameView.setModeChangedCallback([this](EditorMode mode) { onModeChanged(mode); });

    if (static_cast<HWND>(_hostWindow) != nullptr) {
        RECT clientRect{};
        if (GetClientRect(static_cast<HWND>(_hostWindow), &clientRect) != 0) {
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
    bindComponentBrowser();

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
        if (_assetTrash != nullptr) {
            _assetTrash->setProjectRoot(_assetDatabase.projectRoot());
        }
        if (_assetOperations != nullptr) {
            _assetOperations->setProjectRoot(_assetDatabase.projectRoot());
        }
        if (_assetPreviewCache != nullptr) {
            _assetPreviewCache->setDiskCacheRoot(
                (std::filesystem::path(_assetDatabase.projectRoot())
                 / ".ayeditor" / "cache" / "previews").string());
        }
        refreshAssetBrowser();
        if (_assetDatabase.loadedFromIndex()) {
            setAssetBrowserStatus(L"Loaded "
                + std::to_wstring(_assetDatabase.records().size())
                + L" cached assets; checking for changes...");
        } else {
            setAssetBrowserStatus(L"Indexing project assets...");
        }
        (void)_assetDatabase.requestScan();
    }
    reportStartup(0.78f, L"Restoring editor workspace...");

    _mainDock = dynamic_cast<ayt::ui::DockArea*>(_ui.findById("main_dock"));
    if (_mainDock != nullptr) {
        _editorHostServices = std::make_unique<EditorSessionHostServices>(
            *_workspace, _ui, desc.projectRoot,
            _assetPreviewCache.get(), static_cast<HWND>(_hostWindow),
            [this]() {
                if (_repaintCallback) _repaintCallback();
            },
            [this](const std::wstring& text) {
                setAssetBrowserStatus(text);
            },
            [this](std::string_view key, std::wstring_view fallback) {
                const std::string fallbackUtf8 =
                    wideToUtf8(std::wstring(fallback));
                const std::string text = _localization != nullptr
                    ? _localization->get(std::string(key), fallbackUtf8)
                    : fallbackUtf8;
                return ayt::ui::decodeUtf8Text(text);
            },
            [this]() {
                return currentLanguage();
            });
        _dockViewHost = std::make_unique<EditorDockViewHost>(
            *_workspace, *_mainDock, *_editorHostServices, &_ui);
        _dockViewHost->setCloseActionProvider(
            [this](const EditorHostedView& hosted) {
                if (static_cast<HWND>(_hostWindow) == nullptr) {
                    return EditorDocumentCloseAction::Discard;
                }
                const std::wstring prompt =
                    ayt::ui::decodeUtf8Text(hosted.document->title())
                    + L" has unsaved changes.\n\nSave before closing?";
                const int choice = ::MessageBoxW(
                    static_cast<HWND>(_hostWindow), prompt.c_str(), L"AY Editor Document",
                    MB_YESNOCANCEL | MB_ICONWARNING);
                if (choice == IDYES) {
                    return EditorDocumentCloseAction::Save;
                }
                return choice == IDNO
                    ? EditorDocumentCloseAction::Discard
                    : EditorDocumentCloseAction::Cancel;
            });
        _mainDock->setOnCardCloseRequested(
            [this](ayt::ui::DockCard* card) {
                if (card == nullptr || _mainDock == nullptr) {
                    return false;
                }
                if (_dockViewHost != nullptr
                    && _dockViewHost->requestClose(card)) {
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
    std::string projectStartupError = projectStartup.error;
    if (projectStartup) {
        reportStartup(0.86f, L"Opening project startup Scene...");
        if (!_document->open(projectStartup.scenePath, &projectStartupError)) {
            std::fprintf(stderr,
                "[EditorSession] project startup Scene load failed: %s\n",
                projectStartupError.c_str());
        } else {
            std::fprintf(stderr,
                "[EditorSession] opened project startup Scene: %s\n",
                projectStartup.scenePath.c_str());
        }
    } else if (projectStartup.projectDescriptorPresent
               && projectStartupError.empty()) {
        std::fprintf(stderr,
            "[EditorSession] project has no startupWorld; "
            "opened an empty Scene document\n");
    } else if (!projectStartupError.empty()) {
        std::fprintf(stderr,
            "[EditorSession] project startup Scene unavailable: %s\n",
            projectStartupError.c_str());
    }
    selectInitialSceneViewForDocument();
    EditorSelectionContext& sceneSelection =
        _workspace->selections().contextFor("scene.main");
    _selection.bind(&sceneSelection);
    (void)_workspace->selections().activate("scene.main");
    _workspace->commands().setActiveTarget(_document.get());
    syncDocumentCommandMenu();
    _document->setHistoryChangedCallback([this]() {
        ayt::entity::World* world = hierarchyWorldMutable();
        if (!_selection.empty() && _selection.resolve(world) == nullptr) {
            clearSelectedEntity(false);
        }
        _outlinerRefreshPending = true;
        _inspectorRefreshPending = true;
        refreshTransformInspector();
        refreshUnsavedIndicator();
        syncDocumentCommandMenu();
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
    if (!projectStartupError.empty()) {
        setAssetBrowserStatus(
            L"Project startup Scene unavailable: "
                + ayt::ui::decodeUtf8Text(projectStartupError),
            true);
    }
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

    if (hasCrashRecovery()) {
        setAssetBrowserStatus(
            L"A previous editor session ended unexpectedly. Choose the "
            L"documents to recover in the recovery dialog.", true);
        showCrashRecoveryDialog();
    }
    reportStartup(1.0f, L"Editor workspace ready");
    return true;
}

bool EditorSession::initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath) {
    EditorSessionDesc desc;
    desc.uiBackend = backend;
    desc.layoutPath = layoutPath;
    // Keep the compatibility/headless entry point deterministic. Product
    // hosts pass persisted preferences and may opt into "system" locale.
    desc.preferences.language = "en-US";
    return initialize(desc);
}

void EditorSession::shutdown() {
    if (_shutdown) {
        return;
    }
    _shutdown = true;

    finishTransformGizmoDrag(false);
    rememberCurrentSceneView();
    if (_sceneViewWorkspaceDirty) {
        std::string workspaceError;
        if (!_sceneViewWorkspace.save(&workspaceError)) {
            std::fprintf(stderr, "[EditorSceneView] workspace save failed: %s\n",
                         workspaceError.c_str());
        }
        _sceneViewWorkspaceDirty = false;
    }
    savePreferencesNow();
    if (_recoveryStore != nullptr) _recoveryStore->markCleanShutdown();

    _gameView.setModeChangedCallback({});
    _repaintCallback = nullptr;
    // K-INV-D5-6: tear down child HWNDs BEFORE primary UIManager
    // shutdown. ~EditorChildWindowManager calls _wm.destroyTopLevelWindow
    // for every entry; ~UIManager on each child may poke
    // g_activeUIManager if it was active during the last tick. Doing
    // this here (with _ui still alive and owning the active slot)
    // avoids an UAF cleanup race against the primary.
    _audioEditor.reset();
    _audioEditorHandle = nullptr;
    if (_tilemapDockViewHost != nullptr) {
        _tilemapDockViewHost->prepareForUiShutdown();
        _tilemapWindowUiPrepared = true;
    }
    if (_dockViewHost != nullptr) {
        _dockViewHost->prepareForUiShutdown();
    }
    _childWindows.reset();
    if (_tilemapDockViewHost != nullptr) {
        _tilemapDockViewHost->releaseAfterUiShutdown();
        _tilemapDockViewHost.reset();
    }
    _tilemapWindowFrame = nullptr;
    _tilemapWindowDock = nullptr;
    _tilemapWindowHandle = nullptr;
    _tilemapWindowUiPrepared = false;
    _tilemapWindowClosePending = false;
    _tilemapWindowTitle.clear();
    _mainDock = nullptr;
    // Tear down Play/renderer borrow before UI widgets — avoids
    // Inspector path strings and GPU borrows racing UI teardown.
    _playRuntime.shutdownEngine();
    _gameView.setMode(EditorMode::Edit);

    _assetDeleteDialog.reset();
    _assetOperationDialog.reset();
    _assetOperationInput = nullptr;
    _assetOperationFolderPicker = nullptr;
    _recoveryDialog.reset();
    _recoveryChecks.clear();
    _assetTrashDialog.reset();
    _assetTrashList = nullptr;
    _assetHistoryDialog.reset();
    // The application logo borrows its uploaded texture from the shared
    // preview cache. Clear the widget before the cache releases that handle
    // so UI teardown never retains a stale backend resource.
    if (auto* appLogoImage = dynamic_cast<EditorBorrowedImage*>(
            _ui.findById("app_logo_image"))) {
        appLogoImage->clearBorrowedTexture();
    }
    if (_assetInspectorPreview != nullptr) {
        _assetInspectorPreview->setTexture(ayt::ui::ImageTextureHandle{});
    }
    _assetPreviewCache.reset();
    _assetImportQueue.reset();
    _assetImportProgressPercent = -1;

    // v0.3+ PR-5 — Landmine E: 清 Outliner 状态**早于** _ui.shutdown()
    // 避免 _ui.shutdown 期间 _outliner 指向已 free widget（UIManager 析构
    // 链上 deref）。
    _outliner = nullptr;
    _assetTileView = nullptr;
    _assetTree = nullptr;
    _assetSearch = nullptr;
    _assetTypeFilter = nullptr;
    _assetInspectorPreview = nullptr;
    _assetDeleteButton = nullptr;
    _componentPicker = nullptr;
    _attachedComponentPicker = nullptr;
    _componentPropertyBody = nullptr;
    _componentPickerTypeNames.clear();
    _attachedComponentTypeNames.clear();
    _inspectedComponentTypeName.clear();
    _pendingAssetOpenId = 0;
    _saveMenuItem = nullptr;
    _saveAsMenuItem = nullptr;
    _undoMenuItem = nullptr;
    _redoMenuItem = nullptr;
    _restoreDeletedMenuItem = nullptr;
    _restoreRecoveryMenuItem = nullptr;
    _viewportOrientationAxisMenuItem = nullptr;
    _sceneViewModeMenuItem = nullptr;
    _onViewportOrientationAxisVisibilityChanged = {};
    _onPreferencesChanged = {};
    _outlinerEntityIds.clear();
    _assetEntries.clear();
    _selectedAssetIds.clear();
    _assetFolderSourcePaths.clear();
    _assetFolderFlatPaths.clear();
    _assetFolderSourceExpanded.clear();
    _assetDatabase.close();
    _assetTrash.reset();
    _assetOperations.reset();
    _recoveryStore.reset();
    clearSelectedEntity(false);
    _outlinerRefreshPending = false;
    _inspectorRefreshPending = false;
    _outlinerRootExpanded = true;
    _updatingOutlinerSelection = false;
    if (_workspace != nullptr && _dockViewHost == nullptr) {
        _workspace->commands().setActiveTarget(nullptr);
        while (!_workspace->documents().records().empty()) {
            const std::string documentId =
                _workspace->documents().records().back().documentId;
            (void)_workspace->documents().close(
                documentId, EditorDocumentCloseAction::Discard);
        }
    }

    clearSceneUiPreview();
    clearEditorTooltips(this);
    _ui.shutdown();
    if (_dockViewHost != nullptr) {
        _dockViewHost->releaseAfterUiShutdown();
        _dockViewHost.reset();
    }
    _editorHostServices.reset();
    _layoutPath.clear();
    _engineAssetsRoot.clear();
    _projectRoot.clear();
    _projectDefaultSceneView = "Auto";
    _projectEngineProfile.clear();
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
    syncTilemapWindowLifetime();
    if (_tilemapWindowClosePending && _childWindows != nullptr
        && _tilemapWindowHandle != nullptr) {
        const EditorChildWindowManager::Handle handle = _tilemapWindowHandle;
        _tilemapWindowClosePending = false;
        _childWindows->closeChildWindow(handle);
        syncTilemapWindowLifetime();
    }
    if (_tilemapDockViewHost != nullptr) {
        _tilemapDockViewHost->tick(dt);
        refreshTilemapWindowTitle();
    }
    syncUiDesignerLifetime();
    if (_uiDesigner != nullptr) {
        _uiDesigner->pumpDeferred(dt);
    }
    pollUiDesignerProjectChanges(dt);
    syncUiFlowDesignerLifetime();
    if (_uiFlowDesigner != nullptr) {
        _uiFlowDesigner->tick(dt);
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
        syncTilemapWindowLifetime();
    }
    _ui.update(dt);
    pollProjectRunState(dt);
    if (_dockViewHost != nullptr) {
        _dockViewHost->tick(dt);
    }
    // Scene systems and diagnostic hosts can dirty or replace the Edit Scene
    // without going through an EditorSession command. Reconcile the document
    // indicator once per host frame so it cannot remain visually stale.
    refreshUnsavedIndicator();
    syncDocumentCommandMenu();
    _autosaveCountdown -= std::max(0.0f, dt);
    if (_autosaveCountdown <= 0.0f) {
        _autosaveCountdown = 30.0f;
        (void)autosaveNow();
    }

    // v0.3+ PR-5 — Landmine B: 延迟消费 Outliner 重建（禁止在 TreeNode
    // 事件派发内重建；onOutlinerSelectionChanged 注释）。
    if (_outlinerRefreshPending) {
        _outlinerRefreshPending = false;
        refreshOutliner();
    }
    if (_inspectorRefreshPending) {
        _inspectorRefreshPending = false;
        refreshInspectorLabels();
        refreshTransformInspector();
    }
    const bool completedAssetScan = _assetDatabase.pollScan();
    const bool changedAssets = _assetDatabase.pollFileChanges();
    if (completedAssetScan || changedAssets) {
        _assetBrowserRefreshPending = true;
    }
    if (_assetImportQueue != nullptr && _assetImportQueue->poll()) {
        const auto& jobs = _assetImportQueue->jobs();
        const auto completed = std::find_if(jobs.rbegin(), jobs.rend(),
            [](const EditorAssetImportJob& job) { return job.finished(); });
        if (completed != jobs.rend()) {
            if (completed->state == EditorAssetImportJobState::Failed) {
                setAssetBrowserStatus(L"Import failed: "
                    + ayt::ui::decodeUtf8Text(completed->message), true);
            } else {
                if (!completed->outputPaths.empty()) {
                    std::filesystem::path output(completed->outputPaths.front());
                    if (output.is_relative()) output =
                        std::filesystem::path(_assetDatabase.derivedRoot()) / output;
                    _pendingAssetSelectionPath = output.lexically_normal().string();
                }
                _assetCurrentFolder = "Imported";
                (void)_assetDatabase.requestScan();
                setAssetBrowserStatus(completed->state
                    == EditorAssetImportJobState::CacheHit
                    ? L"Import cache reused; refreshing assets."
                    : L"Import complete; refreshing assets.", true);
            }
        }
    }
    if (_assetImportQueue != nullptr && _assetImportQueue->busy()) {
        const int percent = static_cast<int>(std::round(
            _assetImportQueue->overallProgress() * 100.0f));
        if (percent != _assetImportProgressPercent) {
            _assetImportProgressPercent = percent;
            setAssetBrowserStatus(L"Importing assets... "
                + std::to_wstring(percent) + L"%", true);
        }
    } else {
        _assetImportProgressPercent = -1;
    }
    if (_assetPreviewCache != nullptr && _assetPreviewCache->poll()) {
        refreshVisibleAssetPreviews();
        refreshAssetInspector();
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
    if (_pendingAssetOpenId != 0) {
        const EditorAssetId assetId = _pendingAssetOpenId;
        _pendingAssetOpenId = 0;
        (void)openAsset(assetId);
    }
    // Per-frame reconcile: if the last known cursor is not on a splitter
    // band, force every SplitterHandle un-revealed. Leave events alone
    // are not sufficient (capture path / coalesced pointer movement).
    syncSplitterRevealToMouse();

    if (freecamActive()) {
        // Keyboard flight is intentionally gated by the RMB look gesture.
        // Outside that gesture editor shortcuts and text input own the keys.
        if (!_sceneCamera.isTwoD()
            && _sceneCamera.threeD().isLooking()
            && viewportAcceptsGameInput()) {
            if (_devices != nullptr) {
                if (const ayt::device::KeyboardDevice* keyboard =
                        _devices->keyboard()) {
                    const ayt::math::FVector3 eyeBefore =
                        _sceneCamera.threeD().eye();
                    _sceneCamera.threeD().updateMovement(dt, *keyboard);
                    if ((_sceneCamera.threeD().eye() - eyeBefore).lengthSq()
                            > 1.0e-12f) {
                        _sceneViewWorkspaceDirty = true;
                        _sceneViewWorkspaceSaveCountdown = 0.5f;
                    }
                }
            }
        }
        pushSceneCameraToRenderer();
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
    pollSceneViewWorkspace(dt);
}

bool EditorSession::freecamActive() const
{
    return _gameView.mode() == EditorMode::Edit;
}

void EditorSession::pushSceneCameraToRenderer()
{
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        if (_gameView.mode() != EditorMode::Edit) {
            sub->clearCameraOverride();
            sub->clearOverlayCamera2DOverride();
            sub->clearSceneVisibilityFilter();
            sub->renderer().setEditorGrid2DState({});
            return;
        }
        sub->setSceneVisibilityFilter({
            _sceneVisibility.meshes,
            _sceneVisibility.worldLit2D,
            _sceneVisibility.cameraOverlay2D});
        sub->renderer().setWireframeEnabled(_wireframeView);
        const EditorSceneCameraFrame camera = _sceneCamera.frame();
        sub->setCameraMatrices(camera.view, camera.projection, camera.position);
        ayt::render::EditorGrid2DState grid;
        if (_sceneCamera.isTwoD()) {
            // Camera-overlay sprites are screen/game-camera content. They keep
            // the authored OrthoCameraComponent while the Scene View camera
            // observes WorldLit content in world units.
            sub->clearOverlayCamera2DOverride();
            grid.visible = true;
            grid.center = _sceneCamera.twoDCenter();
            grid.verticalWorldSize = _sceneCamera.twoDViewHeight();
            grid.minorSpacing = _sceneCamera.adaptiveGridSpacing();
            grid.majorEvery = 10u;
        } else {
            sub->clearOverlayCamera2DOverride();
        }
        sub->renderer().setEditorGrid2DState(grid);
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
    pushSceneCameraToRenderer();
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
    // panel_viewport. Test their actual pointer hit instead of treating every
    // overlay rectangle as interactive: the authored UI preview deliberately
    // returns nullptr from hitTest() so Scene picking and camera drag pass
    // through it.
    if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
        for (ayt::ui::Widget* child : overlay->getChildren()) {
            if (child == nullptr || !child->isVisible()) {
                continue;
            }
            if (child->hitTest(pos) != nullptr) {
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

bool EditorSession::viewportRay(
    float x, float y,
    ayt::math::FVector3& outOrigin,
    ayt::math::FVector3& outDirection) const
{
    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)
        || viewport.width() <= 0.0f || viewport.height() <= 0.0f
        || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    const ayt::math::FVector2 logical =
        _ui.physicalToLogical(ayt::math::FVector2(x, y));
    return _sceneCamera.ray(
        {logical.x - viewport.minX, logical.y - viewport.minY},
        outOrigin, outDirection);
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
    if (_sceneCamera.threeD().isLooking() || _sceneCamera.isTwoDPanning()) {
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
    if (_dockViewHost != nullptr
        && _dockViewHost->routePointerMove(x, y)) {
        return true;
    }

    updateViewportCoordinateFeedback(x, y);

    if (_sceneCamera.isTwoDPanning()) {
        ayt::math::FRectangle viewport{};
        const ayt::math::FVector2 logical =
            _ui.physicalToLogical({x, y});
        if (getViewportBounds(viewport)
            && _sceneCamera.updateTwoDPan(
                {logical.x - viewport.minX, logical.y - viewport.minY})) {
            _sceneViewWorkspaceDirty = true;
            _sceneViewWorkspaceSaveCountdown = 0.5f;
            pushSceneCameraToRenderer();
            updateViewportCoordinateFeedback(x, y);
            if (_repaintCallback) _repaintCallback();
        }
        return true;
    }

    if (_transformGizmo.active()) {
        return updateTransformGizmoDrag(x, y);
    }

    // Armed viewport LMB: object surfaces only select. A drag past slop is
    // consumed (reserved for future marquee selection) but never rotates the
    // camera; transforms begin exclusively from a gizmo handle.
    if (_viewportLmbPending && !_viewportLmbDragged && !_sceneCamera.threeD().isLooking()) {
        const float dx = x - _viewportLmbX;
        const float dy = y - _viewportLmbY;
        if ((dx * dx + dy * dy)
            >= (kViewportLookDragSlopPixels * kViewportLookDragSlopPixels)) {
            _viewportLmbDragged = true;
            return true;
        }
    }

    if (_sceneCamera.threeD().isLooking()) {
        _sceneCamera.threeD().updateLook(x, y);
        _sceneViewWorkspaceDirty = true;
        _sceneViewWorkspaceSaveCountdown = 0.5f;
        pushSceneCameraToRenderer();
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
        if (_sceneCamera.threeD().isLooking()) {
            _sceneCamera.threeD().endLook();
        }
        _sceneCamera.endTwoDPan();
    }

    if (_dockViewHost != nullptr
        && _dockViewHost->routePointerDown(x, y, button)) {
        return true;
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
            if (_sceneCamera.isTwoD()) {
                ayt::math::FRectangle viewport{};
                const ayt::math::FVector2 logical =
                    _ui.physicalToLogical({x, y});
                if (getViewportBounds(viewport)) {
                    _sceneCamera.beginTwoDPan(
                        {logical.x - viewport.minX,
                         logical.y - viewport.minY});
                }
            } else {
                _sceneCamera.threeD().beginLook(x, y);
            }
            return true;
        }

        if (button == 0) {
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
    if (_dockViewHost != nullptr
        && _dockViewHost->routePointerUp(x, y, button)) {
        return true;
    }
    if (button == 0 && _transformGizmo.active()) {
        updateTransformGizmoDrag(x, y);
        finishTransformGizmoDrag(true);
        _viewportLmbPending = false;
        _viewportLmbDragged = false;
        return true;
    }

    if (_sceneCamera.threeD().isLooking() && button == 1) {
        _sceneCamera.threeD().endLook();
        _sceneViewWorkspaceDirty = true;
        _sceneViewWorkspaceSaveCountdown = 0.5f;
        return true;
    }
    if (_sceneCamera.isTwoDPanning() && button == 1) {
        _sceneCamera.endTwoDPan();
        _sceneViewWorkspaceDirty = true;
        _sceneViewWorkspaceSaveCountdown = 0.5f;
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
    if (_dockViewHost != nullptr
        && _dockViewHost->routeWheel(x, y, deltaY)) {
        return true;
    }
    if (_ui.isCapturing() || isChromePoint(x, y)) {
        return _ui.onMouseWheel(x, y, deltaY);
    }
    if (!std::isfinite(deltaY) || deltaY == 0.0f) {
        return false;
    }
    // The native input bridge has already published Play/Paused wheel input
    // to AYDevice. Consume it as a game-surface gesture without moving the
    // editor camera; runtime camera components stay authoritative.
    if (!freecamActive()) return true;
    // DeviceInputBridge's UI convention is opposite to native wheel notches
    // (+pixels reveals lower content). Convert back before navigating, then
    // dolly along the pointer ray instead of the screen-center forward vector.
    const float wheelNotches = -deltaY / kWheelLogicalPixelsPerNotch;
    if (_sceneCamera.isTwoD()
        || _sceneCamera.projection() == ProjectionMode::Orthographic) {
        ayt::math::FRectangle viewport{};
        if (!getViewportBounds(viewport)) return false;
        const ayt::math::FVector2 logical =
            _ui.physicalToLogical({x, y});
        _sceneCamera.zoomAt(
            std::pow(1.1f, wheelNotches),
            {logical.x - viewport.minX, logical.y - viewport.minY});
    } else {
        ayt::math::FVector3 origin{};
        ayt::math::FVector3 direction{};
        if (!viewportRay(x, y, origin, direction)) return false;
        _sceneCamera.threeD().zoomToward(wheelNotches, direction);
    }
    _sceneViewWorkspaceDirty = true;
    _sceneViewWorkspaceSaveCountdown = 0.5f;
    pushSceneCameraToRenderer();
    updateViewportCoordinateFeedback(x, y);
    if (_repaintCallback) {
        _repaintCallback();
    }
    return true;
}

void EditorSession::onMouseLeave() {
    _hasLastMouse = false;
    if (_sceneCamera.threeD().isLooking()) {
        _sceneCamera.threeD().endLook();
    }
    _sceneCamera.endTwoDPan();
    if (_transformGizmo.active()) {
        finishTransformGizmoDrag(false);
    }
    _gizmoHoverHandle = EditorGizmoHandle::None;
    syncTransformGizmoToRenderer();
    _ui.onMouseLeave();
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_viewport_coordinates"))) {
        label->setText(L"");
    }
    clearSplitterHovers();
}

bool EditorSession::onKeyDown(int keyCode)
{
    if (keyCode == ayt::ui::UIKey_Control) {
        _controlDown = true;
    }
    const ayt::ui::Widget* focused = _ui.getFocusedWidget();
    const bool textEditing = focused != nullptr && focused->isTextEditingWidget();
    if (_dockViewHost != nullptr) {
        _dockViewHost->syncCommandTargetFromFocus();
    }
    if (_workspace != nullptr
        && _workspace->commands().activeTarget() == nullptr) {
        _workspace->commands().setActiveTarget(_document.get());
    }
    IEditorCommandTarget* commandTarget = _workspace != nullptr
        ? _workspace->commands().activeTarget() : nullptr;
    std::uint8_t modifiers = static_cast<std::uint8_t>(
        _ui.getModifiers() & 0x07u);
    if (_controlDown) modifiers |= 0x02u;
    const std::string command =
        EditorShortcutRegistry::instance().commandFor(keyCode, modifiers);
    if (textEditing) {
        if (commandTarget != nullptr && !command.empty()) {
            if (command == "file.save") {
                return executeDocumentCommand("file.save");
            }
            if (command == "dsl.compile") {
                return _workspace->commands().execute(
                    kEditorDslCompileCommand);
            }
        }
    }
    if (!textEditing && _gameView.mode() == EditorMode::Edit) {
        if (command == "edit.undo" || command == "edit.redo") {
            return executeDocumentCommand(command);
        }
        if (command == "file.save") {
            return executeDocumentCommand(command);
        }
    }
    if (!textEditing && command == "edit.delete") {
        if (!_selectedAssetIds.empty() && _assetTileView != nullptr
            && (focused == _assetTileView
                || isDescendantOf(focused, _assetTileView))) {
            requestDeleteSelectedAssets();
            return true;
        }
        if (_dockViewHost != nullptr
            && _dockViewHost->routeKeyDown(keyCode)) {
            return true;
        }
        deleteSelectedEntity();
        return true;
    }
    if (!textEditing && command == "play.toggle") {
        beginOrResumePlay();
        return true;
    }
    if (!textEditing && command == "play.pause") {
        pausePlay();
        return true;
    }
    if (!textEditing && command == "play.stop") {
        stopPlay();
        return true;
    }
    if (!textEditing && modifiers == 0u && _dockViewHost != nullptr
        && _dockViewHost->routeKeyDown(keyCode)) {
        return true;
    }
    return _ui.onKeyDown(keyCode);
}

bool EditorSession::onKeyUp(int keyCode)
{
    if (_dockViewHost != nullptr) {
        (void)_dockViewHost->routeKeyUp(keyCode);
    }
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
    if (_dockViewHost != nullptr) {
        (void)_dockViewHost->routeKeyUp(ayt::ui::UIKey_Space);
        _dockViewHost->releaseInputFocus();
    }
    _ui.cancelCapture();
    finishTransformGizmoDrag(false);
    if (_ui.getFocusedWidget() != nullptr) {
        _ui.setFocus(nullptr);
    }
    if (_sceneCamera.threeD().isLooking()) {
        _sceneCamera.threeD().endLook();
    }
    _viewportLmbPending = false;
    _viewportLmbDragged = false;
    onMouseLeave();
}

bool EditorSession::isUiHoverInteractive() const {
    return _ui.isHoverInteractive();
}

ayt::ui::UiCursorHint EditorSession::getUiCursorHint() const {
    ayt::ui::UiCursorHint hostedHint = ayt::ui::UiCursorHint::Default;
    if (_dockViewHost != nullptr && _hasLastMouse
        && _dockViewHost->resolveCursorHint(
            _lastMouseX, _lastMouseY, hostedHint)
        && hostedHint != ayt::ui::UiCursorHint::Default) {
        return hostedHint;
    }
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

    bindButton("btn_tool_ui_layout", [this]() {
        (void)openUiLayoutEditor();
    });
    bindButton("btn_tool_2d", [this]() {
        (void)openTilemapEditor();
    });
    bindButton("btn_tool_audio", [this]() {
        (void)openRegisteredTool(kEditorAudioToolExtensionId);
    });
    bindButton("btn_run_project", [this]() { (void)runCurrentProject(); });

    // Selection now exposes one Universal transform gizmo. The legacy
    // Select/Move/Rotate/Scale buttons are deliberately not bound even when
    // loading an older custom layout.
    bindButton("btn_tool_space", [this]() {
        setLocalTransformSpace(!_localTransformSpace);
    });
    bindButton("btn_view_mode", [this]() { toggleSceneViewMode(); });
    bindButton("btn_view_camera", [this]() { toggleViewportProjection(); });
    bindButton("btn_view_shading", [this]() { toggleViewportShading(); });
    const auto bindVisibilityButton = [this, &bindButton](
        const char* id, bool EditorSceneVisibility::*member) {
        bindButton(id, [this, member]() {
            EditorSceneVisibility visibility = _sceneVisibility;
            visibility.*member = !(visibility.*member);
            setSceneVisibility(visibility);
        });
    };
    bindVisibilityButton("btn_view_meshes", &EditorSceneVisibility::meshes);
    bindVisibilityButton("btn_view_world_lit_2d",
                         &EditorSceneVisibility::worldLit2D);
    bindVisibilityButton("btn_view_camera_overlay_2d",
                         &EditorSceneVisibility::cameraOverlay2D);
    bindVisibilityButton("btn_view_ui", &EditorSceneVisibility::ui);

    auto* viewportOptions = new ayt::ui::Menu();
    viewportOptions->setId("viewport_options_menu");
    if (ayt::ui::Widget* root = _ui.root()) root->addChild(viewportOptions);
    if (auto* item = viewportOptions->addItem(localizedText(
            "ui.editor.viewport.menu_2d", "2D Scene View"))) {
        item->setId("menu_scene_view_2d");
        item->setOnActivate([this]() { toggleSceneViewMode(); });
        _sceneViewModeMenuItem = item;
    }
    if (auto* item = viewportOptions->addItem(localizedText(
            "ui.editor.viewport.menu_projection", "Perspective Projection"))) {
        item->setId("menu_view_projection");
        item->setOnActivate([this]() { toggleViewportProjection(); });
    }
    if (auto* item = viewportOptions->addItem(localizedText(
            "ui.editor.viewport.menu_wireframe", "Wireframe Rendering"))) {
        item->setId("menu_view_shading");
        item->setOnActivate([this]() { toggleViewportShading(); });
    }
    viewportOptions->addSeparator();
    if (auto* item = viewportOptions->addItem(localizedText(
            "ui.editor.viewport.menu_orientation_axis", "Orientation Axis"))) {
        item->setId("menu_view_orientation_axis");
        item->setOnActivate([this]() {
            setViewportOrientationAxisVisible(
                !_viewportOrientationAxisVisible);
        });
    }
    syncSceneVisibilityMenu();
    bindButton("btn_view_options", [this, viewportOptions]() {
        auto* anchor = _ui.findById("btn_view_options");
        if (anchor == nullptr) return;
        if (viewportOptions->isOpen()) {
            viewportOptions->close();
        } else {
            const auto bounds = anchor->getWorldBounds();
            viewportOptions->open(anchor, {bounds.minX, bounds.maxY + 2.0f});
        }
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
        "btn_view_mode", "btn_view_camera", "btn_view_shading", "btn_view_options",
        "btn_view_meshes", "btn_view_world_lit_2d",
        "btn_view_camera_overlay_2d", "btn_view_ui",
        "btn_tool_ui_layout", "btn_tool_2d", "btn_tool_audio",
        "btn_run_project"
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
    styleLabel("lbl_viewport_coordinates", muted, false);
    styleLabel("lbl_status_scene", muted, false);
    styleLabel("lbl_status_project", muted, false);
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
    const auto attachLocalizedTip = [this](const char* id, const char* key,
                                            const wchar_t* fallback) {
        const std::wstring text = localizedText(key, fallback);
        attachEditorTooltip(this, _ui.findById(id), key, fallback, text);
    };
    attachLocalizedTip("btn_view_camera", "ui.editor.tooltip.projection",
        L"Toggle Perspective / Orthographic projection");
    attachLocalizedTip("btn_view_mode", "ui.editor.tooltip.scene_view",
        L"Switch between 2D and 3D Scene View");
    attachLocalizedTip("btn_view_shading", "ui.editor.tooltip.shading",
        L"Toggle Shaded / Wireframe rendering");
    attachLocalizedTip("btn_view_meshes", "ui.editor.tooltip.meshes",
        L"Show or hide 3D meshes");
    attachLocalizedTip("btn_view_world_lit_2d", "ui.editor.tooltip.world_lit_2d",
        L"Show or hide world-lit 2D content");
    attachLocalizedTip("btn_view_camera_overlay_2d", "ui.editor.tooltip.camera_overlay_2d",
        L"Show or hide camera-overlay 2D content");
    attachLocalizedTip("btn_view_ui", "ui.editor.tooltip.ui_preview",
        L"Show or hide the UI preview");
}

void EditorSession::bindShellIcons(const std::string& iconRootPath)
{
    // The product mark belongs to EngineAssets/AYLogo rather than the
    // third-party command icon set. Bind it independently so every host that
    // consumes the shared editor shell (including AYEditorShell_Demo) gets
    // the same brand treatment.
    if (auto* logo = dynamic_cast<ayt::ui::Panel*>(
            _ui.findById("app_logo"))) {
        bool rasterBound = false;
        if (!_engineAssetsRoot.empty() && _assetPreviewCache != nullptr) {
            const std::filesystem::path rasterPath =
                std::filesystem::path(_engineAssetsRoot)
                / "AYLogo" / "symbol" / "symbol-dark-bg_24px.png";
            std::string error;
            const EditorAuthoringImage image =
                _assetPreviewCache->loadAuthoringImage(
                    rasterPath.string(), &error);
            if (image) {
                auto* raster = new EditorBorrowedImage();
                raster->setId("app_logo_image");
                raster->setBorrowedTexture(image.texture);
                raster->setPosition(ayt::math::FVector2(0.0f, 0.0f));
                raster->setSize(ayt::math::FVector2(24.0f, 24.0f));
                raster->setLayoutPositionManaged(false);
                raster->setLayoutSizeManaged(false);
                raster->setAccessibilityHidden(true);
                logo->addChild(raster);
                rasterBound = true;
            }
        }
        if (!rasterBound && !_engineAssetsRoot.empty()) {
            const std::filesystem::path vectorPath =
                std::filesystem::path(_engineAssetsRoot)
                / "AYLogo" / "symbol" / "symbol-dark-bg_24px.svg";
            std::string error;
            auto document = ayt::ui::SvgDocument::loadFromFile(
                vectorPath, &error);
            if (document != nullptr) {
                auto* vector = new ayt::ui::SvgIcon();
                vector->setId("app_logo_icon");
                vector->setDocument(std::move(document));
                vector->setColor(ayt::math::FVector4(
                    1.0f, 1.0f, 1.0f, 1.0f));
                vector->setPosition(ayt::math::FVector2(0.0f, 0.0f));
                vector->setSize(ayt::math::FVector2(24.0f, 24.0f));
                vector->setLayoutPositionManaged(false);
                vector->setLayoutSizeManaged(false);
                vector->setAccessibilityHidden(true);
                logo->addChild(vector);
            } else {
                std::fprintf(stderr,
                    "[EditorSession] application logo unavailable: %s (%s)\n",
                    vectorPath.string().c_str(), error.c_str());
            }
        }
    }

    if (iconRootPath.empty()) {
        return;
    }

    struct IconBinding {
        const char* buttonId;
        const char* relativePath;
        const char* localizationKey;
        const wchar_t* accessibleLabel;
        float iconSize;
        float horizontalPadding;
        float verticalPadding;
    };

    // Keep this mapping semantic and editor-owned. AYUI owns SVG parsing and
    // drawing; AYEditor decides which visual communicates each command.
    static constexpr IconBinding bindings[] = {
        {"btn_minimize", "outline/minus.svg", "ui.editor.tooltip.minimize", L"Minimize", 13.0f, 5.0f, 3.0f},
        {"btn_maximize", "outline/maximize.svg", "ui.editor.tooltip.maximize", L"Maximize or restore", 13.0f, 5.0f, 3.0f},
        {"btn_close", "outline/x.svg", "ui.editor.tooltip.close", L"Close editor", 13.0f, 5.0f, 3.0f},
        {"btn_play", "filled/player-play.svg", "ui.editor.tooltip.play", L"Play", 16.0f, 8.0f, 4.0f},
        {"btn_pause", "filled/player-pause.svg", "ui.editor.tooltip.pause", L"Pause", 16.0f, 8.0f, 4.0f},
        {"btn_step", "filled/player-track-next.svg", "ui.editor.tooltip.step", L"Step one frame", 16.0f, 8.0f, 4.0f},
        {"btn_stop", "filled/player-stop.svg", "ui.editor.tooltip.stop", L"Stop", 16.0f, 8.0f, 4.0f},
        {"btn_tool_ui_layout", "outline/layout.svg", "ui.editor.tooltip.open_ui_layout", L"Open UI Layout Editor", 20.0f, 7.0f, 7.0f},
        {"btn_tool_2d", "outline/grid.svg", "ui.editor.tooltip.open_tilemap", L"Open 2D Tilemap Editor", 20.0f, 7.0f, 7.0f},
        {"btn_tool_audio", "outline/music-cog.svg", "ui.editor.tooltip.open_audio", L"Open Audio Editor", 20.0f, 7.0f, 7.0f},
        {"btn_run_project", "outline/rocket.svg", "ui.editor.tooltip.run_project", L"Run current project", 20.0f, 7.0f, 7.0f},
        {"btn_tool_space", "outline/world.svg", "ui.editor.tooltip.transform_world", L"Transform orientation: World", 16.0f, 6.0f, 4.0f},
        {"btn_view_mode", "outline/box.svg", "ui.editor.tooltip.view_3d", L"3D Scene View", 16.0f, 6.0f, 4.0f},
        {"btn_view_meshes", "outline/box.svg", "ui.editor.tooltip.show_meshes", L"Show 3D meshes", 15.0f, 5.0f, 4.0f},
        {"btn_view_world_lit_2d", "outline/world.svg", "ui.editor.tooltip.show_world_lit_2d", L"Show world-lit 2D content", 15.0f, 5.0f, 4.0f},
        {"btn_view_camera_overlay_2d", "outline/frame.svg", "ui.editor.tooltip.show_camera_overlay_2d", L"Show camera-overlay 2D content", 15.0f, 5.0f, 4.0f},
        {"btn_view_ui", "outline/layout.svg", "ui.editor.tooltip.show_ui", L"Show UI preview", 15.0f, 5.0f, 4.0f},
        {"btn_view_options", "outline/dots.svg", "ui.editor.tooltip.viewport_options", L"Viewport options", 14.0f, 6.0f, 4.0f},
        {"btn_assets_add", "outline/file-import.svg", "ui.editor.tooltip.import_asset", L"Import asset", 15.0f, 5.0f, 4.0f},
        {"btn_assets_up", "outline/folder-up.svg", "ui.editor.tooltip.parent_folder", L"Go to parent folder", 15.0f, 5.0f, 4.0f},
        {"btn_assets_refresh", "outline/refresh.svg", "ui.editor.tooltip.refresh_assets", L"Refresh assets", 15.0f, 5.0f, 4.0f},
        {"btn_assets_rename", "outline/edit.svg", "ui.editor.tooltip.rename_asset", L"Rename selected asset", 15.0f, 5.0f, 4.0f},
        {"btn_assets_move", "outline/folder-symlink.svg", "ui.editor.tooltip.move_assets", L"Move selected assets", 15.0f, 5.0f, 4.0f},
        {"btn_assets_copy", "outline/copy.svg", "ui.editor.tooltip.copy_assets", L"Copy selected assets", 15.0f, 5.0f, 4.0f},
        {"btn_assets_delete", "outline/trash.svg", "ui.editor.tooltip.delete_assets", L"Delete selected assets", 15.0f, 5.0f, 4.0f},
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
        const std::wstring accessibleText = localizedText(
            binding.localizationKey, binding.accessibleLabel);
        button->setAccessibilityLabel(accessibleText);
        button->setText(L"");
        button->setIconDocument(std::move(document));
        button->setIconSize(binding.iconSize);
        button->setIconColor(iconColor);
        button->setPadding(binding.horizontalPadding, binding.verticalPadding,
                           binding.horizontalPadding, binding.verticalPadding);
        std::wstring tooltipText(accessibleText);
        const char* shortcutId = nullptr;
        if (std::strcmp(binding.buttonId, "btn_play") == 0) shortcutId = "play.toggle";
        else if (std::strcmp(binding.buttonId, "btn_pause") == 0) shortcutId = "play.pause";
        else if (std::strcmp(binding.buttonId, "btn_stop") == 0) shortcutId = "play.stop";
        else if (std::strcmp(binding.buttonId, "btn_assets_delete") == 0) shortcutId = "edit.delete";
        if (shortcutId != nullptr) {
            const std::wstring shortcut =
                EditorShortcutRegistry::instance().shortcutFor(shortcutId);
            if (!shortcut.empty()) tooltipText += L" (" + shortcut + L")";
        }
        const std::wstring suffix = tooltipText.substr(accessibleText.size());
        attachEditorTooltip(this, button, binding.localizationKey,
                            binding.accessibleLabel, tooltipText, suffix);
        ++loadedCount;
    }

    std::fprintf(stderr,
                 "[EditorSession] native SVG icons: %zu/%zu loaded from %s\n",
                 loadedCount, std::size(bindings), root.string().c_str());
    // bindToolbar() establishes the initial visibility state before icons are
    // available. Re-apply it now so each toggle immediately communicates its
    // visible/hidden state through icon tint and accessibility text.
    syncSceneVisibilityMenu();
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
    bindButton("btn_play", [this]() { beginOrResumePlay(); });
    bindButton("btn_pause", [this]() { pausePlay(); });
    bindButton("btn_step", [this]() {
        _gameView.stepOnce();
        if (_repaintCallback) _repaintCallback();
    });
    bindButton("btn_stop", [this]() { stopPlay(); });

    refreshUnsavedIndicator();
    refreshTransformInspector();
}

void EditorSession::beginOrResumePlay()
{
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
                static_cast<HWND>(_hostWindow),
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
}

void EditorSession::pausePlay()
{
    if (_gameView.mode() == EditorMode::Play) {
        _gameView.setMode(EditorMode::Paused);
    }
}

void EditorSession::stopPlay()
{
    if (_gameView.mode() != EditorMode::Edit) {
        _gameView.setMode(EditorMode::Edit);
    }
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
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_document_title"))) {
        label->setText(localizedText(
            "ui.editor.window.document_title",
            "{0}  —  Aliyat Editor", documentTitle));
    }
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_status_scene"))) {
        label->setText(localizedText(
            "ui.editor.status.scene", "Scene: {0}", documentTitle));
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

    auto setText = [this](const char* id, const std::wstring& text) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(text);
            }
        }
    };

    _outlinerEntityIds.clear();

    const ayt::entity::World* world = hierarchyWorld();
    if (world == nullptr) {
        _outliner->clearTree();
        clearSelectedEntity(false);
        setText("outliner_hint", localizedText(
            "ui.editor.outliner.scene", "Scene: {0}", "-"));
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
    setText("outliner_hint", localizedText(
        "ui.editor.outliner.scene", "Scene: {0}", rootLabel));

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
    _assetInspectorPreview = dynamic_cast<ayt::ui::Image*>(
        _ui.findById("inspector_asset_preview"));
    _assetDeleteButton = dynamic_cast<ayt::ui::Button*>(
        _ui.findById("btn_assets_delete"));

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
                const EditorAssetEntry& entry = _assetEntries.at(index);
                const EditorAssetTilePresentation presentation =
                    _assetTilePresenter.present(entry);
                cell.setText(presentation.fullFileName);
                cell.setInfoStrip(
                    presentation.typeAbbreviation,
                    presentation.categoryColor,
                    ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
                cell.setCornerMarkerVisible(
                    presentation.showEngineResourceMarker);
                cell.clearThumbnail();
                if (!entry.folder && _assetPreviewCache != nullptr) {
                    if (const EditorAssetRecord* record =
                            _assetDatabase.find(entry.assetId)) {
                        const ayt::ui::ImageTextureHandle preview =
                            _assetPreviewCache->request(*record);
                        if (preview.isValid()) cell.setThumbnail(preview);
                    }
                }
            });
        _assetTileView->setSelectionMode(
            ayt::ui::TileView::SelectionMode::Extended);
        _assetTileView->setOnSelectionIndicesChanged(
            [this](const std::vector<int>& indices) {
                if (!_updatingAssetSelection) selectAssetsFromIndices(indices);
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
                    if (record != nullptr) {
                        // Do not mutate the DockArea while TileCell is still
                        // dispatching its second mouse-up. The next editor
                        // update opens or focuses the document atomically.
                        _pendingAssetOpenId = entry.assetId;
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
                if (record == nullptr
                    || (record->type != EditorAssetType::Mesh
                        && record->type != EditorAssetType::Texture
                        && record->type != EditorAssetType::Tilemap)) {
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
    bindButton("btn_assets_delete",
               [this]() { requestDeleteSelectedAssets(); });
    bindButton("btn_assets_rename",
               [this]() { requestRenameSelectedAsset(); });
    bindButton("btn_assets_move",
               [this]() { requestRelocateSelectedAssets(false); });
    bindButton("btn_assets_copy",
               [this]() { requestRelocateSelectedAssets(true); });
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
    refreshAssetDeleteButton();

    auto bindViewportAssetDrop = [this](const char* id) {
        ayt::ui::Widget* target = _ui.findById(id);
        if (target == nullptr) return;
        target->setAcceptDrops(true);
        target->setAcceptDropKinds({"EditorAsset"});
        target->setOnDrop([this](const ayt::ui::DragPayload& payload) {
            if (payload.kind != "EditorAsset" || payload.data == nullptr) return;
            const auto* drag = static_cast<const AssetDragData*>(payload.data);
            if (drag != &_assetDragData
                || (drag->type != EditorAssetType::Mesh
                    && drag->type != EditorAssetType::Texture
                    && drag->type != EditorAssetType::Tilemap)) {
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
    std::vector<int> selectedIndices;
    std::vector<EditorAssetId> visibleSelection;
    for (std::size_t i = 0; i < _assetEntries.size(); ++i) {
        if (_assetEntries[i].folder) continue;
        if (std::find(_selectedAssetIds.begin(), _selectedAssetIds.end(),
                      _assetEntries[i].assetId) != _selectedAssetIds.end()) {
            selectedIndices.push_back(static_cast<int>(i));
            visibleSelection.push_back(_assetEntries[i].assetId);
        }
    }
    _selectedAssetIds = std::move(visibleSelection);
    _selectedAssetId = _selectedAssetIds.empty()
        ? 0 : _selectedAssetIds.back();
    _assetTileView->setSelectedIndices(selectedIndices);
    _updatingAssetSelection = false;
    if (_selectedAssetIds.empty()) setInspectorAssetMode(false);
    refreshAssetDeleteButton();
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
    _selectedAssetIds = {assetId};
    setInspectorAssetMode(true);
    refreshAssetInspector();
    refreshAssetDeleteButton();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::selectAssetsFromIndices(const std::vector<int>& indices)
{
    std::vector<EditorAssetId> selected;
    selected.reserve(indices.size());
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(_assetEntries.size())) {
            continue;
        }
        const EditorAssetEntry& entry = _assetEntries[index];
        if (!entry.folder && _assetDatabase.find(entry.assetId) != nullptr) {
            selected.push_back(entry.assetId);
        }
    }
    if (selected.empty()) {
        _selectedAssetId = 0;
        _selectedAssetIds.clear();
        setInspectorAssetMode(false);
    } else {
        // TreeView emits its deselection callback synchronously. Clear the
        // entity side before publishing the new resource selection so that
        // callback cannot erase the resource IDs we are about to select.
        if (_outliner != nullptr && _outliner->getSelectedIndex() >= 0) {
            _outliner->setSelectedIndex(-1);
        }
        clearSelectedEntity(true, false);
        _selectedAssetIds = std::move(selected);
        EditorAssetId primary = 0;
        if (_assetTileView != nullptr) {
            const int focused = _assetTileView->getFocusedIndex();
            if (focused >= 0
                && focused < static_cast<int>(_assetEntries.size())
                && !_assetEntries[focused].folder
                && std::find(_selectedAssetIds.begin(),
                             _selectedAssetIds.end(),
                             _assetEntries[focused].assetId)
                    != _selectedAssetIds.end()) {
                primary = _assetEntries[focused].assetId;
            }
        }
        if (primary == 0) primary = _selectedAssetIds.back();
        _selectedAssetId = primary;
        setInspectorAssetMode(true);
        refreshAssetInspector();
    }
    refreshAssetDeleteButton();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::clearSelectedAsset()
{
    _selectedAssetId = 0;
    _selectedAssetIds.clear();
    setInspectorAssetMode(false);
    if (_assetTileView != nullptr && !_updatingAssetSelection) {
        _updatingAssetSelection = true;
        _assetTileView->clearSelection();
        _updatingAssetSelection = false;
    }
    refreshAssetDeleteButton();
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
    auto set = [this](const char* id, const std::wstring& text) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(_ui.findById(id))) {
            label->setText(text);
        }
    };
    if (_selectedAssetIds.empty()) return;

    if (_selectedAssetIds.size() > 1u) {
        std::uintmax_t totalSize = 0;
        EditorAssetType commonType = EditorAssetType::Unknown;
        EditorAssetOrigin commonOrigin = EditorAssetOrigin::Source;
        bool first = true;
        bool mixedType = false;
        bool mixedOrigin = false;
        for (EditorAssetId id : _selectedAssetIds) {
            const EditorAssetRecord* selected = _assetDatabase.find(id);
            if (selected == nullptr) continue;
            totalSize += selected->size;
            if (first) {
                commonType = selected->type;
                commonOrigin = selected->origin;
                first = false;
            } else {
                mixedType = mixedType || commonType != selected->type;
                mixedOrigin = mixedOrigin || commonOrigin != selected->origin;
            }
        }
        set("inspector_hint", L"Multiple project assets selected");
        set("inspector_asset_header",
            std::to_wstring(_selectedAssetIds.size()) + L" assets selected");
        set("inspector_asset_type", mixedType
            ? L"Type: Mixed" : L"Type: "
                + ayt::ui::decodeUtf8Text(editorAssetTypeName(commonType)));
        set("inspector_asset_origin", mixedOrigin
            ? L"Origin: Mixed" : (commonOrigin == EditorAssetOrigin::Source
                ? L"Origin: Assets (source)"
                : L"Origin: Imported (generated)"));
        set("inspector_asset_size", L"Combined size: "
            + assetSizeText(totalSize));
        set("inspector_asset_modified", L"Modified: Multiple values");
        set("inspector_asset_path", L"Path: Multiple files");
        set("inspector_asset_state", L"State: indexed metadata cached");
        if (_assetInspectorPreview != nullptr) {
            const bool changed = _assetInspectorPreview->isVisible();
            _assetInspectorPreview->setVisible(false);
            if (changed) _ui.invalidateLayout();
        }
        if (auto* reload = dynamic_cast<ayt::ui::Button*>(
                _ui.findById("btn_asset_reload"))) {
            reload->setEnabled(false);
        }
        return;
    }

    const EditorAssetRecord* record = _assetDatabase.find(_selectedAssetId);
    if (record == nullptr) return;
    set("inspector_hint", L"Project asset selected");
    set("inspector_asset_header", ayt::ui::decodeUtf8Text(record->name));
    set("inspector_asset_type", L"Type: "
        + ayt::ui::decodeUtf8Text(editorAssetTypeName(record->type)));
    set("inspector_asset_origin", record->origin == EditorAssetOrigin::Source
        ? L"Origin: Assets (source)" : L"Origin: Imported (generated)");
    set("inspector_asset_size", L"Size: " + assetSizeText(record->size));
    set("inspector_asset_modified", L"Modified: "
        + assetModifiedText(record->lastModified));
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
    std::wstring stateText = std::wstring(L"State: ") + stateName;
    if (record->importState != EditorAssetImportState::NotApplicable) {
        stateText += L" | Import: ";
        stateText += ayt::ui::decodeUtf8Text(
            editorAssetImportStateName(record->importState));
    }
    set("inspector_asset_state", stateText);
    if (auto* reload = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_asset_reload"))) {
        reload->setEnabled(true);
        reload->setText(record->type == EditorAssetType::SourceModel
            ? L"Reimport Source" : L"Reload Asset");
    }

    const bool showPreview = _assetPreviewCache != nullptr
        && EditorAssetPreviewCache::supports(*record);
    if (_assetInspectorPreview != nullptr) {
        const bool visibilityChanged =
            _assetInspectorPreview->isVisible() != showPreview;
        _assetInspectorPreview->setVisible(showPreview);
        if (showPreview) {
            const ayt::ui::ImageTextureHandle preview =
                _assetPreviewCache->request(*record);
            _assetInspectorPreview->setTexture(preview);
        } else {
            _assetInspectorPreview->setTexture(
                ayt::ui::ImageTextureHandle{});
        }
        if (visibilityChanged) _ui.invalidateLayout();
    }
}

void EditorSession::refreshVisibleAssetPreviews()
{
    if (_assetTileView == nullptr || _assetPreviewCache == nullptr) return;
    for (std::size_t index = 0; index < _assetEntries.size(); ++index) {
        if (_assetEntries[index].folder) continue;
        ayt::ui::TileCell* cell = _assetTileView->cellForLogicalIndex(
            static_cast<int>(index));
        const EditorAssetRecord* record =
            _assetDatabase.find(_assetEntries[index].assetId);
        if (cell == nullptr || record == nullptr) continue;
        const ayt::ui::ImageTextureHandle preview =
            _assetPreviewCache->request(*record);
        if (preview.isValid()) cell->setThumbnail(preview);
    }
    if (_repaintCallback) _repaintCallback();
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

void EditorSession::setProjectRunStatus(const std::wstring& summary,
                                        const std::wstring& detail,
                                        bool mirrorToConsole)
{
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_status_project"))) {
        label->setText(summary);
    }
    if (!detail.empty()) {
        setAssetBrowserStatus(detail, mirrorToConsole);
    } else if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::pollProjectRunState(float dtSeconds)
{
    if (_projectProcessId == 0) return;
    _projectProcessPollCountdown -= std::max(0.0f, dtSeconds);
    if (_projectProcessPollCountdown > 0.0f) return;
    _projectProcessPollCountdown = 0.5f;
    if (EditorProjectRunner::processState(_projectProcessId)
        == EditorProjectProcessState::Running) {
        return;
    }
    const std::wstring executable = ayt::ui::decodeUtf8Text(
        _projectExecutableName.empty() ? std::string("Project")
                                       : _projectExecutableName);
    setProjectRunStatus(L"Project: Exited",
        executable + L" exited.", true);
    _projectProcessId = 0;
    _projectExecutableName.clear();
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
    const std::string source = ImportDialog::showOpenAssetFileDialog(static_cast<HWND>(_hostWindow));
    if (source.empty()) return;
    if (!Importer::isSupportedExtension(source)) {
        setAssetBrowserStatus(L"Unsupported import type: "
            + ayt::ui::decodeUtf8Text(source), true);
        return;
    }
    setAssetBrowserStatus(L"Queued import: "
        + ayt::ui::decodeUtf8Text(std::filesystem::path(source).filename().string())
        + L"", true);
    if (_assetImportQueue == nullptr) {
        setAssetBrowserStatus(L"Import queue is unavailable.", true);
        return;
    }
    (void)_assetImportQueue->enqueue(
        source, _assetDatabase.derivedRoot(), false);
}

void EditorSession::reloadSelectedAsset()
{
    if (_selectedAssetIds.size() != 1u) return;
    const EditorAssetRecord* record = _assetDatabase.find(_selectedAssetId);
    if (record == nullptr) return;
    if (record->type == EditorAssetType::SourceModel) {
        if (_assetImportQueue == nullptr) return;
        (void)_assetImportQueue->enqueue(
            record->absolutePath, _assetDatabase.derivedRoot(), true);
        setAssetBrowserStatus(L"Queued reimport: "
            + ayt::ui::decodeUtf8Text(record->name), true);
        return;
    }
    ayt::resource::ResourceManager::instance().reloadResource(
        record->runtimePath);
    refreshAssetInspector();
    setAssetBrowserStatus(L"Reload requested: "
        + ayt::ui::decodeUtf8Text(record->name));
}

bool EditorSession::renameSelectedAsset(const std::string& newFileName)
{
    if (_assetOperations == nullptr || _selectedAssetIds.size() != 1u) {
        return false;
    }
    const EditorAssetRecord* selected =
        _assetDatabase.find(_selectedAssetIds.front());
    if (selected == nullptr) return false;
    const EditorAssetRecord before = *selected;
    ayt::resource::ResourceManager::instance().unloadResource(
        before.runtimePath);
    const EditorAssetOperationResult result = _assetOperations->rename(
        _assetDatabase, before, newFileName);
    if (!result) {
        setAssetBrowserStatus(L"Rename failed: "
            + ayt::ui::decodeUtf8Text(result.error), true);
        return false;
    }
    if (_assetPreviewCache != nullptr) {
        _assetPreviewCache->erase(before.absolutePath);
    }
    clearSelectedAsset();
    _assetCurrentFolder = std::filesystem::path(
        result.resultingLogicalPaths.front()).parent_path().generic_string();
    (void)rescanAssetsNow();
    if (const EditorAssetRecord* renamed = _assetDatabase.findByLogicalPath(
            result.resultingLogicalPaths.front())) {
        selectAsset(renamed->id);
    }
    setAssetBrowserStatus(L"Renamed asset; updated "
        + std::to_wstring(result.updatedReferences) + L" reference(s).", true);
    return true;
}

bool EditorSession::moveSelectedAssets(
    const std::string& destinationLogicalFolder)
{
    if (_assetOperations == nullptr || _selectedAssetIds.empty()) return false;
    std::vector<EditorAssetRecord> records;
    for (EditorAssetId id : _selectedAssetIds) {
        if (const EditorAssetRecord* record = _assetDatabase.find(id)) {
            records.push_back(*record);
        }
    }
    const EditorAssetOperationResult result = _assetOperations->move(
        _assetDatabase, records, destinationLogicalFolder);
    if (!result) {
        setAssetBrowserStatus(L"Move failed: "
            + ayt::ui::decodeUtf8Text(result.error), true);
        return false;
    }
    for (const EditorAssetRecord& record : records) {
        ayt::resource::ResourceManager::instance().unloadResource(
            record.runtimePath);
        if (_assetPreviewCache != nullptr) {
            _assetPreviewCache->erase(record.absolutePath);
        }
    }
    clearSelectedAsset();
    _assetCurrentFolder = destinationLogicalFolder;
    (void)rescanAssetsNow();
    std::vector<int> selectedIndices;
    for (std::size_t index = 0; index < _assetEntries.size(); ++index) {
        if (_assetEntries[index].folder) continue;
        const EditorAssetRecord* record =
            _assetDatabase.find(_assetEntries[index].assetId);
        if (record != nullptr && std::find(result.resultingLogicalPaths.begin(),
                result.resultingLogicalPaths.end(), record->logicalPath)
                != result.resultingLogicalPaths.end()) {
            selectedIndices.push_back(static_cast<int>(index));
        }
    }
    if (_assetTileView != nullptr && !selectedIndices.empty()) {
        _assetTileView->setSelectedIndices(selectedIndices);
        selectAssetsFromIndices(selectedIndices);
    }
    setAssetBrowserStatus(L"Moved " + std::to_wstring(result.affectedAssets)
        + L" asset(s); updated " + std::to_wstring(result.updatedReferences)
        + L" reference(s).", true);
    return true;
}

bool EditorSession::copySelectedAssets(
    const std::string& destinationLogicalFolder)
{
    if (_assetOperations == nullptr || _selectedAssetIds.empty()) return false;
    std::vector<EditorAssetRecord> records;
    for (EditorAssetId id : _selectedAssetIds) {
        if (const EditorAssetRecord* record = _assetDatabase.find(id)) {
            records.push_back(*record);
        }
    }
    const EditorAssetOperationResult result = _assetOperations->copy(
        _assetDatabase, records, destinationLogicalFolder);
    if (!result) {
        setAssetBrowserStatus(L"Copy failed: "
            + ayt::ui::decodeUtf8Text(result.error), true);
        return false;
    }
    clearSelectedAsset();
    _assetCurrentFolder = destinationLogicalFolder;
    (void)rescanAssetsNow();
    setAssetBrowserStatus(L"Copied "
        + std::to_wstring(result.affectedAssets) + L" asset(s).", true);
    return true;
}

void EditorSession::showAssetOperationDialog(
    const std::wstring& titleText, const std::wstring& initialValue,
    bool rename, bool copy)
{
    _assetOperationDialog = std::make_unique<ayt::ui::ModalDialog>();
    _assetOperationDialog->setId("asset_operation_dialog");
    _assetOperationDialog->setSize({460.0f, 184.0f});
    _assetOperationDialog->setAcceptText(copy ? L"Copy" :
        (rename ? L"Rename" : L"Move"));
    _assetOperationDialog->setRejectText(L"Cancel");
    auto* body = new ayt::ui::VBox();
    body->setSpacing(8.0f);
    body->setSize({428.0f, 112.0f});
    auto* title = new ayt::ui::TextLabel();
    title->setText(titleText);
    title->setFontSize(14);
    body->addWidget(title, 26.0f);
    _assetOperationInput = nullptr;
    _assetOperationFolderPicker = nullptr;
    if (rename) {
        _assetOperationInput = new ayt::ui::TextInput();
        _assetOperationInput->setText(initialValue);
        _assetOperationInput->setSize({428.0f, 28.0f});
        _assetOperationInput->selectAll();
        body->addWidget(_assetOperationInput, 28.0f);
    } else {
        auto* hint = new ayt::ui::TextLabel();
        hint->setText(L"Choose a project folder");
        hint->setFontSize(12);
        body->addWidget(hint, 20.0f);
        _assetOperationFolderPicker = new ayt::ui::ComboBox();
        std::vector<std::wstring> folders;
        int selected = -1;
        for (const EditorAssetFolder& folder : _assetDatabase.folders()) {
            if (folder.logicalPath.empty()) continue;
            if (folder.logicalPath == std::filesystem::path(initialValue).generic_string()) {
                selected = static_cast<int>(folders.size());
            }
            folders.push_back(ayt::ui::decodeUtf8Text(folder.logicalPath));
        }
        _assetOperationFolderPicker->setItems(folders);
        if (selected < 0 && !folders.empty()) selected = 0;
        _assetOperationFolderPicker->setSelectedIndex(selected);
        _assetOperationFolderPicker->setSize({428.0f, 28.0f});
        _assetOperationFolderPicker->setMaxPopupItems(10);
        body->addWidget(_assetOperationFolderPicker, 28.0f);
    }
    _assetOperationDialog->setBodyContentOwned(body);
    _assetOperationDialog->setOnResult([this, rename, copy](int result) {
        if (result != ayt::ui::ModalDialog::Ok) return;
        std::string value;
        if (rename && _assetOperationInput != nullptr) {
            value = std::filesystem::path(
                _assetOperationInput->getText()).string();
        } else if (_assetOperationFolderPicker != nullptr) {
            value = std::filesystem::path(
                _assetOperationFolderPicker->getSelectedItem()).generic_string();
        }
        if (value.empty()) return;
        if (rename) (void)renameSelectedAsset(value);
        else if (copy) (void)copySelectedAssets(value);
        else (void)moveSelectedAssets(value);
    });
    _assetOperationDialog->openModal();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::requestRenameSelectedAsset()
{
    if (_selectedAssetIds.size() != 1u) return;
    const EditorAssetRecord* record =
        _assetDatabase.find(_selectedAssetIds.front());
    if (record == nullptr) return;
    showAssetOperationDialog(L"Rename project asset",
        ayt::ui::decodeUtf8Text(record->name), true, false);
}

void EditorSession::requestRelocateSelectedAssets(bool copy)
{
    if (_selectedAssetIds.empty()) return;
    showAssetOperationDialog(copy ? L"Copy to asset folder"
                                  : L"Move to asset folder",
        ayt::ui::decodeUtf8Text(_assetCurrentFolder), false, copy);
}

void EditorSession::refreshAssetDeleteButton()
{
    const bool any = !_selectedAssetIds.empty();
    if (_assetDeleteButton != nullptr) {
        _assetDeleteButton->setEnabled(any);
    }
    const struct { const char* id; bool enabled; } states[] = {
        {"btn_assets_rename", _selectedAssetIds.size() == 1u},
        {"btn_assets_move", any},
        {"btn_assets_copy", any},
    };
    for (const auto& state : states) {
        if (auto* button = dynamic_cast<ayt::ui::Button*>(
                _ui.findById(state.id))) {
            button->setEnabled(state.enabled);
        }
    }
}

void EditorSession::requestDeleteSelectedAssets()
{
    if (_selectedAssetIds.empty()) return;

    const EditorAssetDeleteAnalysis impact = analyzeEditorAssetDeletion(
        _assetDatabase, _selectedAssetIds);

    _assetDeleteDialog = std::make_unique<ayt::ui::ModalDialog>();
    _assetDeleteDialog->setId("asset_delete_confirmation");
    _assetDeleteDialog->setSize(
        impact.hasExternalReferences()
            ? ayt::math::FVector2(520.0f, 326.0f)
            : ayt::math::FVector2(460.0f, 240.0f));
    _assetDeleteDialog->setAcceptText(
        impact.hasExternalReferences() ? L"Delete Anyway" : L"Delete");
    _assetDeleteDialog->setRejectText(L"Cancel");

    auto* body = new ayt::ui::VBox();
    body->setSpacing(8.0f);
    body->setSize(impact.hasExternalReferences()
        ? ayt::math::FVector2(488.0f, 254.0f)
        : ayt::math::FVector2(428.0f, 168.0f));
    auto* title = new ayt::ui::TextLabel();
    title->setText(_selectedAssetIds.size() == 1u
        ? L"Delete this project asset?"
        : L"Delete " + std::to_wstring(_selectedAssetIds.size())
            + L" project assets?");
    title->setFontSize(15);
    body->addWidget(title, 26.0f);

    std::wstring names;
    constexpr std::size_t maxListed = 5u;
    for (std::size_t index = 0;
         index < std::min(maxListed, _selectedAssetIds.size()); ++index) {
        if (const EditorAssetRecord* record =
                _assetDatabase.find(_selectedAssetIds[index])) {
            if (!names.empty()) names += L"\n";
            names += L"• " + ayt::ui::decodeUtf8Text(record->logicalPath);
        }
    }
    if (_selectedAssetIds.size() > maxListed) {
        names += L"\n• and "
            + std::to_wstring(_selectedAssetIds.size() - maxListed)
            + L" more";
    }
    auto* list = new ayt::ui::TextLabel();
    list->setText(names);
    list->setSize({428.0f, 104.0f});
    body->addWidget(list, 104.0f);

    if (impact.hasExternalReferences()) {
        std::wstring dependencies = L"Used by:\n";
        constexpr std::size_t maxReferences = 5u;
        for (std::size_t index = 0;
             index < std::min(maxReferences, impact.references.size());
             ++index) {
            if (index != 0u) dependencies += L"\n";
            dependencies += L"• " + ayt::ui::decodeUtf8Text(
                impact.references[index].referencingPath);
        }
        if (impact.references.size() > maxReferences) {
            dependencies += L"\n• and "
                + std::to_wstring(impact.references.size() - maxReferences)
                + L" more references";
        }
        auto* references = new ayt::ui::TextLabel();
        references->setId("asset_delete_references");
        references->setText(dependencies);
        references->setSize({488.0f, 72.0f});
        references->setTextColor(
            ayt::math::FVector4(1.0f, 0.70f, 0.28f, 1.0f));
        body->addWidget(references, 72.0f);
    }

    auto* warning = new ayt::ui::TextLabel();
    warning->setText(impact.hasExternalReferences()
        ? L"Referenced files may break. Files can be restored from Edit."
        : L"Files move to the project trash and can be restored from Edit.");
    warning->setSize({428.0f, 22.0f});
    body->addWidget(warning, 22.0f);
    _assetDeleteDialog->setBodyContentOwned(body);
    _assetDeleteDialog->setOnResult([this](int result) {
        if (result == ayt::ui::ModalDialog::Ok) {
            deleteSelectedAssetsConfirmed();
        }
    });
    _assetDeleteDialog->openModal();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::deleteSelectedAssetsConfirmed()
{
    std::vector<EditorAssetRecord> records;
    records.reserve(_selectedAssetIds.size());
    for (EditorAssetId id : _selectedAssetIds) {
        if (const EditorAssetRecord* record = _assetDatabase.find(id)) {
            records.push_back(*record);
        }
    }

    for (const EditorAssetRecord& record : records) {
        ayt::resource::ResourceManager::instance().unloadResource(
            record.runtimePath);
    }
    const EditorAssetTrashResult result = _assetTrash != nullptr
        ? _assetTrash->moveToTrash(records)
        : EditorAssetTrashResult{0u, "Project trash is unavailable."};
    if (result) {
        for (const EditorAssetRecord& record : records) {
            if (_assetPreviewCache != nullptr) {
                _assetPreviewCache->erase(record.absolutePath);
            }
        }
    }

    clearSelectedAsset();
    (void)rescanAssetsNow();
    if (_restoreDeletedMenuItem != nullptr) {
        _restoreDeletedMenuItem->setEnabled(
            _assetTrash != nullptr && _assetTrash->canRestoreLast());
    }
    if (result) {
        setAssetBrowserStatus(
            L"Moved " + std::to_wstring(result.moved)
            + (result.moved == 1u ? L" asset to project trash."
                                  : L" assets to project trash."), true);
    } else {
        setAssetBrowserStatus(
            L"Asset deletion failed: "
            + ayt::ui::decodeUtf8Text(result.error), true);
    }
}

bool EditorSession::restoreLastDeletedAssets()
{
    if (_assetTrash == nullptr) return false;
    const EditorAssetTrashResult result = _assetTrash->restoreLast();
    if (_restoreDeletedMenuItem != nullptr) {
        _restoreDeletedMenuItem->setEnabled(_assetTrash->canRestoreLast());
    }
    if (!result) {
        setAssetBrowserStatus(L"Restore failed: "
            + ayt::ui::decodeUtf8Text(result.error), true);
        return false;
    }
    (void)rescanAssetsNow();
    setAssetBrowserStatus(L"Restored " + std::to_wstring(result.moved)
        + (result.moved == 1u ? L" asset." : L" assets."), true);
    return true;
}

bool EditorSession::runCurrentProject()
{
    if (_projectProcessId != 0) {
        const EditorProjectProcessState state =
            EditorProjectRunner::processState(_projectProcessId);
        if (state == EditorProjectProcessState::Running) {
            const bool focused = EditorProjectRunner::focusProcessWindow(
                _projectProcessId);
            const std::wstring executable = ayt::ui::decodeUtf8Text(
                _projectExecutableName.empty() ? std::string("Project")
                                               : _projectExecutableName);
            setProjectRunStatus(L"Project: Running",
                executable + L" is already running (process "
                + std::to_wstring(_projectProcessId) + L")."
                + (focused ? L" Focused its window."
                           : L" Its window is not ready yet."), true);
            return true;
        }
        _projectProcessId = 0;
        _projectExecutableName.clear();
    }

    std::string error;
    const EditorProjectRunConfig config = EditorProjectRunner::resolve(
        _assetDatabase.projectRoot(), &error);
    if (!config) {
        const std::wstring message = L"Run project: "
            + ayt::ui::decodeUtf8Text(error);
        setProjectRunStatus(L"Project: Not runnable", message, true);
        ::MessageBoxW(static_cast<HWND>(_hostWindow), message.c_str(), L"Run Project Failed",
                      MB_OK | MB_ICONERROR);
        return false;
    }
    const std::string executableName =
        std::filesystem::path(config.executable).filename().string();
    setProjectRunStatus(L"Project: Starting",
        L"Starting " + ayt::ui::decodeUtf8Text(config.executable) + L"...",
        true);
    const EditorProjectLaunchResult launched =
        EditorProjectRunner::launch(config);
    if (!launched) {
        const std::wstring message = L"Run project failed: "
            + ayt::ui::decodeUtf8Text(launched.error);
        setProjectRunStatus(L"Project: Failed", message, true);
        ::MessageBoxW(static_cast<HWND>(_hostWindow), message.c_str(), L"Run Project Failed",
                      MB_OK | MB_ICONERROR);
        return false;
    }
    _projectProcessId = launched.processId;
    _projectExecutableName = executableName;
    _projectProcessPollCountdown = 0.5f;
    setProjectRunStatus(L"Project: Running",
        ayt::ui::decodeUtf8Text(executableName) + L" started (process "
        + std::to_wstring(launched.processId) + L").", true);
    return true;
}

void EditorSession::showAssetTrashDialog()
{
    if (_assetTrash == nullptr || _assetTrash->transactions().empty()) {
        setAssetBrowserStatus(L"Project trash is empty.");
        return;
    }
    const auto transactions = _assetTrash->transactions();
    _assetTrashDialog = std::make_unique<ayt::ui::ModalDialog>();
    _assetTrashDialog->setId("asset_trash_dialog");
    _assetTrashDialog->setSize({680.0f, 420.0f});
    _assetTrashDialog->setAcceptText(L"Clear Selected Permanently");
    _assetTrashDialog->setRejectText(L"Close");
    auto* body = new ayt::ui::VBox();
    body->setSpacing(8.0f);
    body->setSize({648.0f, 348.0f});
    auto* title = new ayt::ui::TextLabel();
    title->setText(L"Project Trash");
    title->setFontSize(15);
    body->addWidget(title, 28.0f);
    auto* hint = new ayt::ui::TextLabel();
    hint->setText(L"Ctrl/Shift selects multiple deletion transactions.");
    hint->setFontSize(12);
    body->addWidget(hint, 22.0f);
    _assetTrashList = new ayt::ui::ListView();
    _assetTrashList->setSelectionMode(
        ayt::ui::ListView::SelectionMode::Extended);
    std::vector<std::wstring> items;
    for (const EditorAssetTrashTransaction& transaction : transactions) {
        std::string label = transaction.id + "  —  "
            + std::to_string(transaction.entries.size()) + " item(s)";
        if (!transaction.entries.empty()) {
            label += "  —  " + std::filesystem::path(
                transaction.entries.front().originalPath).filename().string();
        }
        items.push_back(ayt::ui::decodeUtf8Text(label));
    }
    _assetTrashList->setItems(items);
    _assetTrashList->setSize({648.0f, 230.0f});
    body->addWidget(_assetTrashList, 230.0f);
    auto* restore = new ayt::ui::Button();
    restore->setText(L"Restore Selected");
    restore->setStyleId("editor_property_button");
    restore->setSize({150.0f, 28.0f});
    restore->setOnClicked([this, transactions]() {
        if (_assetTrash == nullptr || _assetTrashList == nullptr) return;
        std::size_t restored = 0;
        for (const int index : _assetTrashList->getSelectedIndices()) {
            if (index < 0 || static_cast<std::size_t>(index) >= transactions.size()) continue;
            const EditorAssetTrashResult result = _assetTrash->restore(
                transactions[static_cast<std::size_t>(index)].id);
            if (!result) {
                setAssetBrowserStatus(L"Restore failed: "
                    + ayt::ui::decodeUtf8Text(result.error), true);
                return;
            }
            restored += result.moved;
        }
        (void)rescanAssetsNow();
        setAssetBrowserStatus(L"Restored " + std::to_wstring(restored)
            + L" asset(s) from project trash.", true);
        if (_assetTrashDialog != nullptr) _assetTrashDialog->rejectDialog();
    });
    body->addWidget(restore, 28.0f);
    _assetTrashDialog->setBodyContentOwned(body);
    _assetTrashDialog->setOnResult([this, transactions](int result) {
        if (result != ayt::ui::ModalDialog::Ok || _assetTrash == nullptr
            || _assetTrashList == nullptr) return;
        const auto selected = _assetTrashList->getSelectedIndices();
        if (selected.empty()) return;
#if defined(_WIN32)
        if (::MessageBoxW(static_cast<HWND>(_hostWindow),
                L"Permanently delete the selected trash transactions? This cannot be undone.",
                L"Clear Project Trash", MB_YESNO | MB_ICONWARNING)
            != IDYES) return;
#endif
        std::size_t purged = 0;
        for (const int index : selected) {
            if (index < 0 || static_cast<std::size_t>(index) >= transactions.size()) continue;
            const EditorAssetTrashResult cleared = _assetTrash->purge(
                transactions[static_cast<std::size_t>(index)].id);
            if (!cleared) {
                setAssetBrowserStatus(L"Trash clear failed: "
                    + ayt::ui::decodeUtf8Text(cleared.error), true);
                return;
            }
            purged += cleared.moved;
        }
        setAssetBrowserStatus(L"Permanently cleared "
            + std::to_wstring(purged) + L" asset(s).", true);
    });
    _assetTrashDialog->openModal();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::showAssetHistoryDialog()
{
    const auto history = readEditorAssetOperationHistory(
        _assetDatabase.projectRoot());
    _assetHistoryDialog = std::make_unique<ayt::ui::ModalDialog>();
    _assetHistoryDialog->setId("asset_history_dialog");
    _assetHistoryDialog->setSize({760.0f, 420.0f});
    _assetHistoryDialog->setAcceptText(L"Close");
    _assetHistoryDialog->setRejectText(L"Close");
    auto* body = new ayt::ui::VBox();
    body->setSpacing(8.0f);
    body->setSize({728.0f, 348.0f});
    auto* title = new ayt::ui::TextLabel();
    title->setText(L"Resource Operation History");
    title->setFontSize(15);
    body->addWidget(title, 28.0f);
    auto* list = new ayt::ui::ListView();
    std::vector<std::wstring> items;
    if (history.empty()) items.push_back(L"No resource operations recorded.");
    for (const EditorAssetOperationHistoryEntry& entry : history) {
        std::string label = entry.operation + "  |  " + entry.source;
        if (!entry.destination.empty()) label += "  ->  " + entry.destination;
        if (!entry.error.empty()) label += "  |  " + entry.error;
        items.push_back(ayt::ui::decodeUtf8Text(label));
    }
    list->setItems(items);
    list->setSize({728.0f, 292.0f});
    body->addWidget(list, 292.0f);
    _assetHistoryDialog->setBodyContentOwned(body);
    _assetHistoryDialog->openModal();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::validateProjectContent()
{
    const EditorRuntimeValidationResult headless =
        EditorProjectRuntimeValidator::validate(
            _assetDatabase.projectRoot(),
            EditorRuntimeValidationProfile::Headless);
    const EditorRuntimeValidationResult client =
        EditorProjectRuntimeValidator::validate(
            _assetDatabase.projectRoot(),
            EditorRuntimeValidationProfile::FullClient);
    if (!headless || !client) {
        const EditorRuntimeValidationIssue* issue = !headless.issues.empty()
            ? &headless.issues.front()
            : (!client.issues.empty() ? &client.issues.front() : nullptr);
        setAssetBrowserStatus(L"Content validation failed"
            + (issue != nullptr ? L": "
                + ayt::ui::decodeUtf8Text(issue->path + " - " + issue->message)
                : std::wstring{}), true);
        return;
    }
    setAssetBrowserStatus(L"Validated "
        + std::to_wstring(client.scenes) + L" Scene, "
        + std::to_wstring(client.uiLayouts) + L" UI and "
        + std::to_wstring(client.uiFlows) + L" UI Flow, "
        + std::to_wstring(client.tilemaps)
        + L" Tilemap asset(s) for headless and full client.", true);
}

bool EditorSession::autosaveNow()
{
    if (_recoveryStore == nullptr) return false;
    std::vector<const IEditorDocument*> documents;
    if (_document != nullptr) documents.push_back(_document.get());
    if (_workspace != nullptr) {
        for (const EditorDocumentRecord& record :
             _workspace->documents().records()) {
            if (record.document != nullptr) {
                documents.push_back(record.document.get());
            }
        }
    }
    const EditorRecoveryResult result = _recoveryStore->autosave(documents);
    if (!result && !result.error.empty()) {
        setAssetBrowserStatus(L"Autosave warning: "
            + ayt::ui::decodeUtf8Text(result.error), true);
    }
    return result.error.empty();
}

bool EditorSession::hasCrashRecovery() const noexcept
{
    return _recoveryStore != nullptr
        && _recoveryStore->hasRecoverableSession();
}

bool EditorSession::restoreCrashRecovery()
{
    if (_recoveryStore == nullptr) return false;
    std::vector<std::size_t> indices;
    const auto documents = _recoveryStore->recoverableDocuments();
    indices.reserve(documents.size());
    for (std::size_t index = 0; index < documents.size(); ++index) {
        indices.push_back(index);
    }
    return restoreCrashRecovery(indices);
}

bool EditorSession::restoreCrashRecovery(
    const std::vector<std::size_t>& indices)
{
    if (_recoveryStore == nullptr) return false;
    const std::string openScenePath = _document != nullptr
        ? _document->path() : std::string{};
    const EditorRecoveryResult result = _recoveryStore->restorePrevious(indices);
    if (_restoreRecoveryMenuItem != nullptr) {
        _restoreRecoveryMenuItem->setEnabled(hasCrashRecovery());
    }
    if (!result) {
        setAssetBrowserStatus(L"Recovery failed: "
            + ayt::ui::decodeUtf8Text(result.error), true);
        return false;
    }
    if (!openScenePath.empty() && _document != nullptr) {
        std::string ignored;
        if (_document->open(openScenePath, &ignored)) afterDocumentReload();
    }
    (void)rescanAssetsNow();
    setAssetBrowserStatus(L"Restored " + std::to_wstring(result.documents)
        + L" autosaved document(s). Original files were backed up with a "
          L".before-recovery suffix.", true);
    return true;
}

void EditorSession::showCrashRecoveryDialog()
{
    if (_recoveryStore == nullptr) return;
    const auto documents = _recoveryStore->recoverableDocuments();
    if (documents.empty()) return;
    _recoveryChecks.clear();
    _recoveryDialog = std::make_unique<ayt::ui::ModalDialog>();
    _recoveryDialog->setId("crash_recovery_dialog");
    const float height = std::min(440.0f,
        150.0f + 28.0f * static_cast<float>(documents.size()));
    _recoveryDialog->setSize({560.0f, height});
    _recoveryDialog->setAcceptText(L"Restore Selected");
    _recoveryDialog->setRejectText(L"Later");
    auto* body = new ayt::ui::VBox();
    body->setSpacing(7.0f);
    body->setSize({528.0f, height - 64.0f});
    auto* title = new ayt::ui::TextLabel();
    title->setText(L"Recover documents from the previous editor session");
    title->setFontSize(15);
    body->addWidget(title, 28.0f);
    auto* hint = new ayt::ui::TextLabel();
    hint->setText(L"Existing files receive a .before-recovery backup.");
    hint->setFontSize(12);
    body->addWidget(hint, 22.0f);
    for (const EditorRecoveryDocument& document : documents) {
        auto* check = new ayt::ui::CheckBox();
        check->setChecked(true);
        const std::string label = document.title.empty()
            ? document.originalPath : document.title + "  —  " + document.originalPath;
        check->setText(ayt::ui::decodeUtf8Text(label));
        check->setSize({528.0f, 24.0f});
        body->addWidget(check, 24.0f);
        _recoveryChecks.push_back(check);
    }
    _recoveryDialog->setBodyContentOwned(body);
    _recoveryDialog->setOnResult([this](int result) {
        if (result != ayt::ui::ModalDialog::Ok) return;
        std::vector<std::size_t> selected;
        for (std::size_t index = 0; index < _recoveryChecks.size(); ++index) {
            if (_recoveryChecks[index] != nullptr
                && _recoveryChecks[index]->isChecked()) selected.push_back(index);
        }
        if (selected.empty()) {
            setAssetBrowserStatus(L"No recovery documents selected.", true);
            return;
        }
        (void)restoreCrashRecovery(selected);
    });
    _recoveryDialog->openModal();
    if (_repaintCallback) _repaintCallback();
}

bool EditorSession::createProjectAsset(EditorAssetType type)
{
    const EditorProjectAssetCreateResult created = createEditorProjectAsset(
        _assetDatabase.projectRoot(), type);
    if (!created) {
        setAssetBrowserStatus(L"Create asset failed: "
            + ayt::ui::decodeUtf8Text(created.error), true);
        return false;
    }
    _pendingAssetSelectionPath = created.absolutePath;
    _assetCurrentFolder = std::filesystem::path(created.logicalPath)
        .parent_path().generic_string();
    (void)rescanAssetsNow();
    const EditorAssetRecord* record =
        _assetDatabase.findByLogicalPath(created.logicalPath);
    const bool opened = record != nullptr && openAsset(record->id);
    setAssetBrowserStatus(
        (opened ? L"Created and opened " : L"Created ")
        + ayt::ui::decodeUtf8Text(created.logicalPath));
    return true;
}

bool EditorSession::openAsset(EditorAssetId assetId)
{
    const EditorAssetRecord* record = _assetDatabase.find(assetId);
    if (record == nullptr) return false;
    switch (record->type) {
    case EditorAssetType::Scene: {
        if (_document == nullptr || _gameView.mode() != EditorMode::Edit) {
            return false;
        }
        if (_document->isDirty()) {
            const int choice = ::MessageBoxW(
                static_cast<HWND>(_hostWindow),
                L"The current scene has unsaved changes.\n\nDiscard them and open another scene?",
                L"AYEditor", MB_YESNO | MB_ICONWARNING);
            if (choice != IDYES) return false;
        }
        std::string error;
        if (!_document->open(record->absolutePath, &error)) {
            setAssetBrowserStatus(L"Scene open failed: "
                + ayt::ui::decodeUtf8Text(error), true);
            return false;
        }
        afterDocumentReload();
        setAssetBrowserStatus(L"Opened scene: "
            + ayt::ui::decodeUtf8Text(record->logicalPath));
        return true;
    }
    case EditorAssetType::UiLayout:
        return openUiLayoutEditor(record->absolutePath);
    case EditorAssetType::UiFlow:
        return openUiFlowEditor(record->absolutePath);
    case EditorAssetType::GameFlow:
        return openGameFlowEditor(record->absolutePath);
    case EditorAssetType::Animation:
    case EditorAssetType::Audio: {
        if (_dockViewHost == nullptr) return false;
        EditorOpenRequest request;
        request.resourcePath = record->absolutePath;
        request.resourceKey = record->absolutePath;
        request.displayPath = record->logicalPath;
        request.assetType = editorAssetTypeName(record->type);
        request.preferredEditorId = record->type == EditorAssetType::Animation
            ? kEditorAnimationTimelineExtensionId
            : kEditorAudioTimelineExtensionId;
        EditorDockViewOptions options;
        options.cardId = "card_timed_asset_" + std::to_string(assetId);
        const EditorDockOpenResult opened = _dockViewHost->open(request, options);
        if (!opened) {
            setAssetBrowserStatus(L"Timeline asset open failed: "
                + ayt::ui::decodeUtf8Text(opened.error), true);
            return false;
        }
        (void)openRegisteredTool(kEditorTimelineToolExtensionId);
        wirePromoteCallback();
        _ui.invalidateLayout();
        setAssetBrowserStatus(L"Opened timeline asset: "
            + ayt::ui::decodeUtf8Text(record->logicalPath));
        return true;
    }
    case EditorAssetType::Tilemap: {
        if (_dockViewHost == nullptr) return false;
        EditorOpenRequest request;
        request.resourcePath = record->absolutePath;
        request.resourceKey = record->absolutePath;
        request.displayPath = record->logicalPath;
        request.assetType = "Tilemap";
        request.preferredEditorId = kEditorTilemapExtensionId;
        EditorDockViewOptions options;
        options.cardId = "card_tilemap_" + std::to_string(assetId);
        const EditorDockOpenResult opened = _dockViewHost->open(request, options);
        if (!opened) {
            setAssetBrowserStatus(L"Tilemap open failed: "
                + ayt::ui::decodeUtf8Text(opened.error), true);
            return false;
        }
        wirePromoteCallback();
        _ui.invalidateLayout();
        setAssetBrowserStatus(L"Opened tilemap: "
            + ayt::ui::decodeUtf8Text(record->logicalPath));
        return true;
    }
    default:
        return openDslAsset(assetId);
    }
}

bool EditorSession::ensureTilemapWindow()
{
    syncTilemapWindowLifetime();
    if (_tilemapDockViewHost != nullptr && _tilemapWindowHandle != nullptr) {
        _tilemapWindowClosePending = false;
        (void)_childWindows->activateChildWindow(_tilemapWindowHandle);
        return true;
    }
    if (_childWindows == nullptr || _workspace == nullptr
        || _editorHostServices == nullptr) {
        return false;
    }

    ChildWindowConfig cfg;
    cfg.title = "2D Tilemap Editor";
    cfg.x = 112;
    cfg.y = 82;
    cfg.width = 1280;
    cfg.height = 800;

    // The dedicated tool owns editor-painted chrome just like a promoted
    // DockCard, while its content remains a private DockArea. This separates
    // window movement (outer frame title) from document movement (inner
    // tabs), and gives future cross-tool-window tab merging a common DockArea
    // boundary instead of tying the surface to native Win32 chrome.
    auto windowFrame = std::make_unique<ayt::ui::DockCard>();
    windowFrame->setId("tilemap_window_frame");
    windowFrame->setTitle(L"2D Tilemap Editor");
    windowFrame->setHeaderHeight(28.0f);
    windowFrame->setClosable(true);
    windowFrame->setFloatable(true);
    auto* dock = new ayt::ui::DockArea();
    dock->setId("tilemap_window_dock");
    windowFrame->setContent(dock);
    cfg.card = windowFrame.get();
    cfg.redockable = false;
    cfg.beforeMouseButton = [this](
        ayt::ui::UIManager&, float x, float y,
        int button, bool pressed) {
        if (_tilemapDockViewHost == nullptr) return false;
        return pressed
            ? _tilemapDockViewHost->routePointerDown(x, y, button)
            : _tilemapDockViewHost->routePointerUp(x, y, button);
    };
    cfg.beforeMouseMove = [this](
        ayt::ui::UIManager&, float x, float y) {
        return _tilemapDockViewHost != nullptr
            && _tilemapDockViewHost->routePointerMove(x, y);
    };
    cfg.beforeMouseWheel = [this](
        ayt::ui::UIManager&, float x, float y, float deltaY) {
        return _tilemapDockViewHost != nullptr
            && _tilemapDockViewHost->routeWheel(x, y, deltaY);
    };
    cfg.beforeKey = [this](ayt::ui::UIManager& ui,
                           ayt::device::KeyCode key, bool pressed) {
        return routeTilemapWindowKey(ui, key, pressed);
    };
    cfg.onFocusChanged = [this](ayt::ui::UIManager&, bool focused) {
        if (_tilemapDockViewHost == nullptr) return;
        if (focused) {
            if (const EditorHostedView* active =
                    _tilemapDockViewHost->active()) {
                (void)_tilemapDockViewHost->activate(active->documentId);
            }
        } else {
            (void)_tilemapDockViewHost->routeKeyUp(ayt::ui::UIKey_Space);
            _tilemapDockViewHost->releaseInputFocus();
        }
    };
    cfg.resolveCursorHint = [this](
        ayt::ui::UIManager&, float x, float y) {
        ayt::ui::UiCursorHint hint = ayt::ui::UiCursorHint::Default;
        if (_tilemapDockViewHost != nullptr) {
            (void)_tilemapDockViewHost->resolveCursorHint(x, y, hint);
        }
        return hint;
    };
    cfg.beforeCloseRequested = [this](ayt::ui::UIManager&) {
        return confirmTilemapWindowClose();
    };
    cfg.beforeClose = [this](ayt::ui::UIManager&) {
        if (_tilemapDockViewHost != nullptr) {
            _tilemapDockViewHost->prepareForUiShutdown();
            _tilemapWindowUiPrepared = true;
        }
        _tilemapWindowFrame = nullptr;
        _tilemapWindowDock = nullptr;
        _tilemapWindowHandle = nullptr;
    };

    EditorChildWindowManager::Handle handle = nullptr;
    if (!_childWindows->openChildWindow(cfg, handle) || handle == nullptr) {
        setAssetBrowserStatus(
            L"2D Tilemap Editor window creation failed", true);
        return false;
    }
    // The child UI root now owns the outer frame and its DockArea content.
    windowFrame.release();
    ayt::ui::UIManager* childUi = _childWindows->uiForHandle(handle);
    if (childUi == nullptr || childUi->root() == nullptr) {
        _childWindows->closeChildWindow(handle);
        setAssetBrowserStatus(L"2D Tilemap Editor UI host failed", true);
        return false;
    }

    _tilemapWindowHandle = handle;
    _tilemapWindowFrame = cfg.card;
    _tilemapWindowDock = dock;
    _tilemapWindowUiPrepared = false;
    _tilemapWindowClosePending = false;
    _tilemapDockViewHost = std::make_unique<EditorDockViewHost>(
        *_workspace, *dock, *_editorHostServices, childUi);
    _tilemapDockViewHost->setCloseActionProvider(
        [this](const EditorHostedView& hosted) {
            if (hosted.document == nullptr || !hosted.document->isDirty()) {
                return EditorDocumentCloseAction::Discard;
            }
            HWND owner = _tilemapWindowHandle != nullptr
                ? static_cast<HWND>(_tilemapWindowHandle) : static_cast<HWND>(_hostWindow);
            if (owner == nullptr) return EditorDocumentCloseAction::Discard;
            const std::wstring prompt = ayt::ui::decodeUtf8Text(
                hosted.document->title())
                + L" has unsaved changes.\n\nSave before closing?";
            const int choice = ::MessageBoxW(
                owner, prompt.c_str(), L"2D Tilemap Editor",
                MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDYES) return EditorDocumentCloseAction::Save;
            return choice == IDNO
                ? EditorDocumentCloseAction::Discard
                : EditorDocumentCloseAction::Cancel;
        });
    dock->setOnCardCloseRequested([this](ayt::ui::DockCard* card) {
        if (_tilemapDockViewHost == nullptr || card == nullptr) return false;
        const bool handled = _tilemapDockViewHost->requestClose(card);
        if (handled && _tilemapDockViewHost->count() == 0u) {
            _tilemapWindowClosePending = true;
        }
        return handled;
    });
    childUi->invalidateLayout();
    childUi->layout();
    return true;
}

void EditorSession::syncTilemapWindowLifetime()
{
    if (_tilemapDockViewHost == nullptr) return;
    bool alive = false;
    if (_childWindows != nullptr && _tilemapWindowHandle != nullptr) {
        for (const auto& entry : _childWindows->entries()) {
            if (entry.handle == _tilemapWindowHandle) {
                alive = true;
                break;
            }
        }
    }
    if (alive) return;

    // beforeClose prepares view callbacks while the child UI tree is still
    // alive. The manager has now destroyed that tree; only release the
    // widget-free hosted records here.
    if (_tilemapWindowUiPrepared) {
        _tilemapDockViewHost->releaseAfterUiShutdown();
    }
    _tilemapDockViewHost.reset();
    _tilemapWindowFrame = nullptr;
    _tilemapWindowDock = nullptr;
    _tilemapWindowHandle = nullptr;
    _tilemapWindowUiPrepared = false;
    _tilemapWindowClosePending = false;
    _tilemapWindowTitle.clear();
}

bool EditorSession::confirmTilemapWindowClose()
{
    if (_tilemapDockViewHost == nullptr || _workspace == nullptr) return true;
    struct PendingClose {
        std::string documentId;
        std::shared_ptr<IEditorDocument> document;
        bool save = false;
    };
    std::vector<PendingClose> pending;
    for (const EditorDocumentRecord& record :
         _workspace->documents().records()) {
        if (_tilemapDockViewHost->find(record.documentId) == nullptr) continue;
        PendingClose close{record.documentId, record.document, false};
        if (record.document != nullptr && record.document->isDirty()) {
            HWND owner = _tilemapWindowHandle != nullptr
                ? static_cast<HWND>(_tilemapWindowHandle) : static_cast<HWND>(_hostWindow);
            if (owner != nullptr) {
                const std::wstring prompt = ayt::ui::decodeUtf8Text(
                    record.document->title())
                    + L" has unsaved changes.\n\nSave before closing?";
                const int choice = ::MessageBoxW(
                    owner, prompt.c_str(), L"2D Tilemap Editor",
                    MB_YESNOCANCEL | MB_ICONWARNING);
                if (choice == IDCANCEL) return false;
                close.save = choice == IDYES;
            }
        }
        pending.push_back(std::move(close));
    }

    // Save every requested document before removing any tab. A failed save
    // therefore keeps the window and all of its documents intact.
    for (const PendingClose& close : pending) {
        if (!close.save || close.document == nullptr) continue;
        std::string error;
        bool saved = false;
        try {
            saved = close.document->save(&error);
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "Document save raised an unknown exception.";
        }
        if (!saved) {
            setAssetBrowserStatus(L"Tilemap save failed: "
                + ayt::ui::decodeUtf8Text(error), true);
            return false;
        }
    }
    for (const PendingClose& close : pending) {
        const EditorCloseResult result = _tilemapDockViewHost->close(
            close.documentId, EditorDocumentCloseAction::Discard);
        if (!result) return false;
    }
    return true;
}

bool EditorSession::routeTilemapWindowKey(
    ayt::ui::UIManager& ui, ayt::device::KeyCode key, bool pressed)
{
    if (_tilemapDockViewHost == nullptr || _workspace == nullptr) return false;
    const int uiKey = static_cast<int>(ayt::ui::fromDeviceKey(key));
    if (!pressed) return _tilemapDockViewHost->routeKeyUp(uiKey);

    if (const EditorHostedView* active = _tilemapDockViewHost->active()) {
        (void)_tilemapDockViewHost->activate(active->documentId);
    }
    _tilemapDockViewHost->syncCommandTargetFromFocus();
    const ayt::ui::Widget* focused = ui.getFocusedWidget();
    const bool textEditing = focused != nullptr
        && focused->isTextEditingWidget();
    const std::uint8_t modifiers = static_cast<std::uint8_t>(
        ui.getModifiers() & 0x07u);
    const std::string command = EditorShortcutRegistry::instance()
        .commandFor(uiKey, modifiers);
    if ((!textEditing && (command == "edit.undo"
                          || command == "edit.redo"))
        || command == "file.save") {
        return _workspace->commands().execute(command);
    }
    if (!textEditing && modifiers == 0u) {
        return _tilemapDockViewHost->routeKeyDown(uiKey);
    }
    return false;
}

void EditorSession::refreshTilemapWindowTitle()
{
    if (_childWindows == nullptr || _tilemapWindowHandle == nullptr
        || _tilemapDockViewHost == nullptr) return;
    std::string title = "2D Tilemap Editor";
    if (const EditorHostedView* active = _tilemapDockViewHost->active()) {
        if (active->document != nullptr) {
            title += " - " + active->document->title();
            if (active->document->isDirty()) title += " *";
        }
    }
    if (title == _tilemapWindowTitle) return;
    if (_tilemapWindowFrame != nullptr) {
        _tilemapWindowFrame->setTitle(ayt::ui::decodeUtf8Text(title));
        _tilemapWindowFrame->markDirty();
    }
    if (_childWindows->setChildWindowTitle(_tilemapWindowHandle, title)) {
        _tilemapWindowTitle = std::move(title);
    }
}

bool EditorSession::openTilemapEditor(const std::string& path)
{
    if (_workspace == nullptr) {
        setAssetBrowserStatus(L"2D Tilemap Editor is unavailable", true);
        return false;
    }

    EditorDockViewHost* targetHost = _dockViewHost.get();
    bool dedicatedWindow = false;
    if (_childWindows != nullptr && _editorHostServices != nullptr
        && ensureTilemapWindow()) {
        targetHost = _tilemapDockViewHost.get();
        dedicatedWindow = targetHost != nullptr;
    }
    if (targetHost == nullptr) {
        setAssetBrowserStatus(L"2D Tilemap Editor host is unavailable", true);
        return false;
    }

    EditorOpenRequest request;
    request.resourcePath = path;
    request.resourceKey = path.empty()
        ? "workspace:tilemap:untitled" : path;
    request.displayPath = path.empty() ? "Untitled Tilemap" : path;
    request.assetType = "Tilemap";
    request.preferredEditorId = kEditorTilemapExtensionId;

    EditorDockViewOptions options;
    if (path.empty()) options.cardId = "card_tilemap_workspace";
    const EditorDockOpenResult opened = targetHost->open(request, options);
    if (!opened) {
        setAssetBrowserStatus(L"2D Tilemap Editor open failed: "
            + ayt::ui::decodeUtf8Text(opened.error), true);
        if (dedicatedWindow && targetHost->count() == 0u) {
            _tilemapWindowClosePending = true;
        }
        return false;
    }
    if (dedicatedWindow) {
        if (_tilemapWindowHandle != nullptr) {
            (void)_childWindows->activateChildWindow(_tilemapWindowHandle);
        }
        if (ayt::ui::UIManager* childUi =
                _childWindows->uiForHandle(_tilemapWindowHandle)) {
            childUi->invalidateLayout();
            childUi->layout();
        }
        refreshTilemapWindowTitle();
    } else {
        wirePromoteCallback();
        _ui.invalidateLayout();
    }
    setAssetBrowserStatus(opened.document.status
            == EditorOpenStatus::FocusedExisting
        ? L"2D Tilemap Editor focused"
        : dedicatedWindow
            ? L"2D Tilemap Editor opened in a dedicated window"
            : L"2D Tilemap Editor opened");
    if (_repaintCallback) _repaintCallback();
    return true;
}

bool EditorSession::openRegisteredTool(const std::string& editorId)
{
    if (_dockViewHost == nullptr || _workspace == nullptr) return false;
    const EditorDescriptor* descriptor = _workspace->registry().find(editorId);
    if (descriptor == nullptr
        || descriptor->surfaceKind != EditorSurfaceKind::ToolPanel) {
        setAssetBrowserStatus(L"Tool is not registered", true);
        return false;
    }
    EditorOpenRequest request;
    request.resourceKey = "tool:" + editorId;
    request.preferredEditorId = editorId;
    EditorDockViewOptions options;
    options.cardId = "card_tool_" + editorId;
    const EditorDockOpenResult opened = _dockViewHost->open(request, options);
    if (!opened) {
        setAssetBrowserStatus(L"Tool open failed: "
            + ayt::ui::decodeUtf8Text(opened.error), true);
        return false;
    }
    wirePromoteCallback();
    _ui.invalidateLayout();
    if (_repaintCallback) _repaintCallback();
    return true;
}

bool EditorSession::openDslAsset(EditorAssetId assetId)
{
    if (_dockViewHost == nullptr) return false;
    const EditorAssetRecord* record = _assetDatabase.find(assetId);
    if (record == nullptr
        || editorDslLanguageFromPath(record->name)
               == EditorDslLanguage::Unknown) {
        return false;
    }

    EditorOpenRequest request;
    request.resourcePath = record->absolutePath;
    request.resourceKey = record->absolutePath;
    request.displayPath = record->logicalPath;
    request.preferredEditorId = kEditorDslExtensionId;

    EditorDockViewOptions options;
    options.cardId = "card_dsl_" + std::to_string(assetId);
    const EditorDockOpenResult opened = _dockViewHost->open(request, options);
    if (!opened) {
        setAssetBrowserStatus(
            L"DSL open failed: " + ayt::ui::decodeUtf8Text(opened.error), true);
        return false;
    }

    // DSL cards are created after the initial shell traversal. Wire the new
    // live card immediately so it has the same tear-off behaviour as cards
    // loaded from editor_shell.ui.json.
    wirePromoteCallback();
    _ui.invalidateLayout();
    setAssetBrowserStatus(
        L"Opened DSL: " + ayt::ui::decodeUtf8Text(record->logicalPath));
    if (_repaintCallback) _repaintCallback();
    return true;
}

bool EditorSession::placeAssetInViewport(EditorAssetId assetId,
                                         float physicalX, float physicalY)
{
    const EditorAssetRecord* record = _assetDatabase.find(assetId);
    if (record == nullptr || _document == nullptr
        || _gameView.mode() != EditorMode::Edit) {
        return false;
    }
    const bool isMesh = record->type == EditorAssetType::Mesh;
    const bool isTexture = record->type == EditorAssetType::Texture;
    const bool isTilemap = record->type == EditorAssetType::Tilemap;
    if ((!isMesh && !isTexture && !isTilemap)
        || ((isTexture || isTilemap) && !_sceneCamera.isTwoD())) {
        return false;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    if (world == nullptr) return false;
    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    if (!viewportRay(physicalX, physicalY, origin, direction)) return false;

    ayt::math::FVector3 position = origin + direction * 5.0f;
    const float planeDirection = _sceneCamera.isTwoD()
        ? direction.z : direction.y;
    const float planeOrigin = _sceneCamera.isTwoD() ? origin.z : origin.y;
    if (std::fabs(planeDirection) > 1.0e-5f) {
        const float distance = -planeOrigin / planeDirection;
        if (distance >= 0.0f) position = origin + direction * distance;
    }

    std::string materialPath;
    ayt::math::FVector3 initialScale{1.0f, 1.0f, 1.0f};
    std::string componentAssetPath;
    if (isMesh) {
        std::shared_ptr<ayt::resource::IMesh> meshResource;
        try {
            meshResource = ayt::resource::ResourceManager::instance()
                .load<ayt::resource::IMesh>(record->runtimePath);
        } catch (...) {
            // Preserve the reference so Reload can retry after replacement.
        }
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
        componentAssetPath = _assetDatabase.portableAssetPath(*record);
    } else if (isTexture) {
        componentAssetPath = _assetDatabase.portableAssetPath(*record);
        initialScale = {100.0f, 100.0f, 1.0f};
        try {
            const auto texture = ayt::resource::ResourceManager::instance()
                .load<ayt::resource::ITexture>(record->runtimePath);
            if (texture != nullptr && texture->getWidth() > 0u
                && texture->getHeight() > 0u) {
                initialScale.x = static_cast<float>(texture->getWidth());
                initialScale.y = static_cast<float>(texture->getHeight());
            }
        } catch (...) {
        }
    } else {
        componentAssetPath = tilemapRuntimeReference(_assetDatabase, *record);
        try {
            const auto tilemap = ayt::resource::ResourceManager::instance()
                .load<ayt::resource::ITilemap>(editorRuntimeAssetPath(
                    _assetDatabase, componentAssetPath));
            if (tilemap != nullptr) {
                position.x -= static_cast<float>(tilemap->getCols())
                    * static_cast<float>(tilemap->getTileWidth()) * 0.5f;
                position.y -= static_cast<float>(tilemap->getRows())
                    * static_cast<float>(tilemap->getTileHeight()) * 0.5f;
            }
        } catch (...) {
        }
    }

    const std::string stem = std::filesystem::path(record->name).stem().string();
    const char* fallbackName = isMesh ? "Mesh" : (isTexture ? "Sprite" : "Tilemap");
    uint32_t entityId = 0;
    if (!_document->createEntity(
            std::string("Create ") + fallbackName,
            [&](ayt::entity::Entity& candidate) {
                const std::string name =
                    (stem.empty() ? std::string(fallbackName) : stem)
                    + " " + std::to_string(
                        static_cast<unsigned>(candidate.getId()));
                candidate.setName(name.c_str());
                auto* transform =
                    candidate.addComponent<ayt::entity::Transform>();
                if (transform == nullptr) return false;
                transform->setPosition(position.x, position.y, position.z);
                transform->setScale(
                    initialScale.x, initialScale.y, initialScale.z);
                if (isMesh) {
                    auto* mesh =
                        candidate.addComponent<ayt::entity::MeshComponent>();
                    if (mesh == nullptr) return false;
                    mesh->meshPath = componentAssetPath;
                    mesh->materialPath =
                        _assetDatabase.portableAssetPath(materialPath);
                } else if (isTexture) {
                    auto* sprite = candidate.addComponent<
                        ayt::entity::SpriteComponent>();
                    if (sprite == nullptr) return false;
                    sprite->texturePath = componentAssetPath;
                } else {
                    auto* tilemap = candidate.addComponent<
                        ayt::entity::TilemapComponent>();
                    if (tilemap == nullptr) return false;
                    tilemap->tilemapPath = componentAssetPath;
                }
                return true;
            },
            &entityId)) {
        return false;
    }
    ayt::entity::Entity* entity = world->findEntity(entityId);
    if (entity == nullptr) return false;

    setSelectedEntity(world, entity);
    _inspectedComponentTypeName = isMesh ? "MeshComponent"
        : (isTexture ? "SpriteComponent" : "TilemapComponent");
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

std::wstring EditorSession::localizedText(const char* key,
                                          const char* fallback) const
{
    const std::string text = _localization != nullptr
        ? _localization->get(key, fallback)
        : std::string(fallback);
    return ayt::ui::decodeUtf8Text(text);
}

std::wstring EditorSession::localizedText(
    std::string_view key, std::wstring_view fallback) const
{
    const std::wstring fallbackText(fallback);
    const std::string text = _localization != nullptr
        ? _localization->get(std::string(key), wideToUtf8(fallbackText))
        : wideToUtf8(fallbackText);
    return ayt::ui::decodeUtf8Text(text);
}

std::wstring EditorSession::localizedText(const char* key,
                                          const char* fallback,
                                          const std::string& argument) const
{
    const std::string text = _localization != nullptr
        ? _localization->formatWithFallback(key, fallback, argument)
        : std::string(fallback);
    if (_localization == nullptr) {
        const std::size_t marker = text.find("{0}");
        if (marker != std::string::npos) {
            std::string expanded = text;
            expanded.replace(marker, 3u, argument);
            return ayt::ui::decodeUtf8Text(expanded);
        }
    }
    return ayt::ui::decodeUtf8Text(text);
}

void EditorSession::setLocalizedValueLabel(const char* widgetId,
                                           const char* key,
                                           const char* fallback,
                                           const char* formattedValue)
{
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById(widgetId))) {
        label->setText(localizedText(
            key, fallback, std::string(formattedValue)));
    }
}

void EditorSession::refreshLocalizedValueLabels()
{
    struct SliderLabel {
        const char* sliderId;
        const char* labelId;
        const char* key;
        const char* fallback;
        const char* numberFormat;
    };
    static constexpr SliderLabel labels[] = {
        {"sld_gamma", "lbl_gamma", "ui.editor.render.gamma_value",
         "Gamma  {0}", "%.2f"},
        {"sld_exposure", "lbl_exposure", "ui.editor.render.exposure_value",
         "Exposure  {0}", "%.2f"},
        {"sld_bloom", "lbl_bloom", "ui.editor.render.bloom_value",
         "Bloom  {0}", "%.2f"},
        {"sld_haze_strength", "lbl_haze_strength",
         "ui.editor.render.haze_strength_value", "Haze Strength  {0}", "%.2f"},
        {"sld_haze_density", "lbl_haze_density",
         "ui.editor.render.haze_density_value", "Haze Density  {0}", "%.3f"},
        {"sld_ssao_strength", "lbl_ssao_strength",
         "ui.editor.render.ssao_strength_value", "SSAO Strength  {0}", "%.2f"},
        {"sld_ssao_radius", "lbl_ssao_radius",
         "ui.editor.render.ssao_radius_value", "SSAO Radius  {0}", "%.2f"},
        {"sld_ssao_bias", "lbl_ssao_bias",
         "ui.editor.render.ssao_bias_value", "SSAO Bias  {0}", "%.3f"},
        {"sld_ambient", "lbl_ambient", "ui.editor.render.ambient_value",
         "IBL Ambient  {0}", "%.2f"},
        {"sld_shadow_bias", "lbl_shadow_bias",
         "ui.editor.render.shadow_bias_value", "Shadow Bias  {0}", "%.4f"},
        {"sld_color_grading_strength", "lbl_color_grading_strength",
         "ui.editor.render.color_grading_strength_value",
         "Color Grade Strength  {0}", "%.2f"},
    };
    for (const SliderLabel& binding : labels) {
        auto* slider = dynamic_cast<ayt::ui::Slider*>(
            _ui.findById(binding.sliderId));
        if (slider == nullptr) continue;
        char value[32]{};
        std::snprintf(value, sizeof(value), binding.numberFormat,
                      static_cast<double>(slider->getValue()));
        setLocalizedValueLabel(binding.labelId, binding.key,
                               binding.fallback, value);
    }
    if (auto* slider = dynamic_cast<ayt::ui::Slider*>(
            _ui.findById("sld_net_hp"))) {
        char value[32]{};
        std::snprintf(value, sizeof(value), "%.0f",
                      static_cast<double>(slider->getValue()));
        setLocalizedValueLabel("lbl_net_hp", "ui.editor.network.cube_hp",
                               "Cube HP  {0}", value);
    }
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
                std::snprintf(buf, sizeof(buf), "%.0f",
                              static_cast<double>(v));
                setLocalizedValueLabel(
                    "lbl_net_hp", "ui.editor.network.cube_hp",
                    "Cube HP  {0}", buf);
            });
        }
    }
    refreshLocalizedValueLabels();
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

    auto bindSlider = [this](const char* id, std::function<void(float)> onChanged) {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                slider->setOnValueChanged(std::move(onChanged));
            }
        }
    };

    bindSlider("sld_gamma", [this, rendererOrNull](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel("lbl_gamma", "ui.editor.render.gamma_value",
                               "Gamma  {0}", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessGamma(v);
        }
    });

    bindSlider("sld_exposure", [this, rendererOrNull](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_exposure", "ui.editor.render.exposure_value",
            "Exposure  {0}", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessExposure(v);
        }
    });

    bindSlider("sld_bloom", [this, rendererOrNull](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel("lbl_bloom", "ui.editor.render.bloom_value",
                               "Bloom  {0}", buf);
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

    bindSlider("sld_haze_strength", [this, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_haze_strength", "ui.editor.render.haze_strength_value",
            "Haze Strength  {0}", buf);
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

    bindSlider("sld_haze_density", [this, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_haze_density", "ui.editor.render.haze_density_value",
            "Haze Density  {0}", buf);
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

    bindSlider("sld_ssao_strength", [this, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_ssao_strength", "ui.editor.render.ssao_strength_value",
            "SSAO Strength  {0}", buf);
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

    bindSlider("sld_ssao_radius", [this, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_ssao_radius", "ui.editor.render.ssao_radius_value",
            "SSAO Radius  {0}", buf);
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

    bindSlider("sld_ssao_bias", [this, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_ssao_bias", "ui.editor.render.ssao_bias_value",
            "SSAO Bias  {0}", buf);
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

    bindSlider("sld_ambient", [this, rendererOrNull](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_ambient", "ui.editor.render.ambient_value",
            "IBL Ambient  {0}", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setAmbientStrength(v);
        }
    });

    bindSlider("sld_shadow_bias", [this, rendererOrNull](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
        setLocalizedValueLabel(
            "lbl_shadow_bias", "ui.editor.render.shadow_bias_value",
            "Shadow Bias  {0}", buf);
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
                    if (auto* taa = dynamic_cast<ayt::ui::CheckBox*>(
                            _ui.findById("chk_taa"))) {
                        taa->setChecked(false);
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
                    if (auto* taa = dynamic_cast<ayt::ui::CheckBox*>(
                            _ui.findById("chk_taa"))) {
                        taa->setChecked(false);
                    }
                }
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setSmaaEnabled(on);
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_taa")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, rendererOrNull](bool on) {
                if (on) {
                    if (auto* fxaa = dynamic_cast<ayt::ui::CheckBox*>(
                            _ui.findById("chk_fxaa"))) {
                        fxaa->setChecked(false);
                    }
                    if (auto* smaa = dynamic_cast<ayt::ui::CheckBox*>(
                            _ui.findById("chk_smaa"))) {
                        smaa->setChecked(false);
                    }
                }
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setTaaEnabled(on);
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
               [this, applyColorGrading](float value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value));
        setLocalizedValueLabel(
            "lbl_color_grading_strength",
            "ui.editor.render.color_grading_strength_value",
            "Color Grade Strength  {0}", buf);
        applyColorGrading();
    });

    // Labels in JSON are decorative until Slider min/max/value load;
    // refresh from the live widget values so thumb ↔ text stay aligned.
    refreshLocalizedValueLabels();
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
    bool taaEnabled = false;
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
    if (auto* w = _ui.findById("chk_taa")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            taaEnabled = chk->isChecked();
        }
    }
    r.setTaaEnabled(taaEnabled);
    r.setSmaaEnabled(smaaEnabled && !taaEnabled);
    r.setFxaaEnabled(fxaaEnabled && !smaaEnabled && !taaEnabled);
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
        _viewportOrientationAxisMenuItem->setText(localizedText(
            "ui.editor.menu.view.orientation_axis", "Viewport Orientation Axis"));
        setEditorMenuToggleIcon(_viewportOrientationAxisMenuItem,
                                _engineAssetsRoot,
                                _viewportOrientationAxisVisible);
    }
    if (preferences.cameraPoseValid) {
        _sceneCamera.threeD().setPose(preferences.cameraEye,
                         preferences.cameraYawRadians,
                         preferences.cameraPitchRadians);
    }
    if (std::isfinite(preferences.cameraMoveSpeed)
        && preferences.cameraMoveSpeed > 0.01f) {
        _sceneCamera.threeD().setMoveSpeed(preferences.cameraMoveSpeed);
    }
    setActiveTool(preferences.activeTool);
    setLocalTransformSpace(preferences.localTransformSpace);
    _sceneCamera.setThreeDProjection(preferences.orthographicView
        ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
    _wireframeView = preferences.wireframeView;
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_shading"))) {
        button->setText(_wireframeView
            ? localizedText("ui.editor.viewport.wireframe", "Wireframe")
            : localizedText("ui.editor.viewport.shaded", "Shaded"));
    }
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_shading"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_wireframe", "Wireframe Rendering"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot, _wireframeView);
    }
    syncSceneViewToolbar();
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_orientation_axis"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_orientation_axis", "Orientation Axis"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot,
                                _viewportOrientationAxisVisible);
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
    setCheck("chk_taa", preferences.taaEnabled);
    setCheck("chk_fxaa", preferences.fxaaEnabled
        && !preferences.smaaEnabled && !preferences.taaEnabled);
    setCheck("chk_smaa", preferences.smaaEnabled && !preferences.taaEnabled);
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
    pushSceneCameraToRenderer();
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
    out.cameraEye = _sceneCamera.threeD().eye();
    out.cameraYawRadians = _sceneCamera.threeD().yawRadians();
    out.cameraPitchRadians = _sceneCamera.threeD().pitchRadians();
    out.cameraMoveSpeed = _sceneCamera.threeD().moveSpeed();
    out.activeTool = _activeTool;
    out.localTransformSpace = _localTransformSpace;
    out.orthographicView =
        _sceneCamera.threeDProjection() == ProjectionMode::Orthographic;
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
    out.taaEnabled = checkValue("chk_taa", out.taaEnabled);
    if (out.taaEnabled) {
        out.fxaaEnabled = false;
        out.smaaEnabled = false;
    } else if (out.smaaEnabled) {
        out.fxaaEnabled = false;
    }
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

std::string EditorSession::currentLanguage() const
{
    return _localization != nullptr
        ? _localization->currentLanguage() : std::string{};
}

bool EditorSession::setLanguage(const std::string& language)
{
    if (_localization == nullptr) return false;
    const std::string requested = language.empty() ? "system" : language;
    const std::string locale = requested == "system"
        ? systemLanguageTag() : requested;
    const std::string resolved =
        _localization->resolveSupportedLanguage(locale);
    if (resolved.empty()) return false;

    _localization->setLanguage(locale);
    _preferences.language = requested;
    _ui.loader().retranslate(_ui.root());
    if (const auto found = gEditorMenuTexts.find(this);
        found != gEditorMenuTexts.end()) {
        for (const EditorMenuTextBinding& binding : found->second) {
            const std::wstring text = localizedText(
                binding.key, binding.fallback);
            if (binding.item != nullptr) binding.item->setText(text);
            if (binding.menuBar != nullptr) {
                binding.menuBar->setMenuTitle(binding.menuIndex, text);
            }
        }
    }
    if (const auto found = gEditorTooltips.find(this);
        found != gEditorTooltips.end()) {
        for (const EditorTooltipBinding& binding : found->second) {
            if (binding.tooltip != nullptr) {
                binding.tooltip->setText(localizedText(
                    binding.key, binding.fallback) + binding.suffix);
            }
        }
    }
    if (_dockViewHost != nullptr) {
        _dockViewHost->notifyLanguageChanged(resolved);
    }
    if (_tilemapDockViewHost != nullptr) {
        _tilemapDockViewHost->notifyLanguageChanged(resolved);
    }
    if (_uiDesigner != nullptr) {
        _uiDesigner->onLanguageChanged();
    }
    if (_childWindows != nullptr) {
        for (const EditorChildWindowManager::Entry& entry
             : _childWindows->entries()) {
            if (entry.ui != nullptr) {
                entry.ui->loader().retranslate(entry.ui->root());
                entry.ui->invalidateLayout();
                entry.ui->layout();
            }
        }
    }

    // These labels encode live editor state, so their key depends on the
    // current value rather than only on the original layout property.
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_camera"))) {
        const bool orthographic = _sceneCamera.isTwoD()
            || _sceneCamera.threeDProjection() == ProjectionMode::Orthographic;
        button->setText(orthographic
            ? localizedText("ui.editor.viewport.orthographic", "Orthographic")
            : localizedText("ui.editor.viewport.perspective", "Perspective"));
    }
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_shading"))) {
        button->setText(_wireframeView
            ? localizedText("ui.editor.viewport.wireframe", "Wireframe")
            : localizedText("ui.editor.viewport.shaded", "Shaded"));
    }

    refreshUnsavedIndicator();
    refreshOutliner();
    refreshLocalizedValueLabels();
    syncSceneViewToolbar();
    _ui.invalidateLayout();
    _ui.layout();
    if (_repaintCallback) _repaintCallback();
    savePreferencesNow();
    return true;
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
    defaults.language = _preferences.language;
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
    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("lbl_active_tool"))) {
        label->setText(localizedText(
            "ui.editor.tool.universal", "Universal"));
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
        const std::wstring accessibleLabel = local
            ? localizedText("ui.editor.accessibility.transform_local",
                            "Transform orientation: Local")
            : localizedText("ui.editor.accessibility.transform_world",
                            "Transform orientation: World");
        button->setAccessibilityLabel(accessibleLabel);

        // Production shells bind the World icon before preferences are
        // applied. Keep text as the fallback for test/custom shells that do
        // not provide an icon root, and swap the glyph only for icon-backed
        // buttons.
        if (button->getIconDocument() != nullptr) {
            const std::filesystem::path iconPath =
                std::filesystem::path(_engineAssetsRoot)
                / "Icons" / "Tabler" / "outline"
                / (local ? "box.svg" : "world.svg");
            std::string error;
            auto icon = ayt::ui::SvgDocument::loadFromFile(iconPath, &error);
            if (icon != nullptr) {
                button->setText(L"");
                button->setIconDocument(std::move(icon));
            } else {
                button->setIconDocument({});
                button->setText(local ? L"L" : L"W");
            }
        } else {
            button->setText(local
                ? localizedText("ui.editor.tool.local", "Local")
                : localizedText("ui.editor.tool.world", "World"));
        }
    }
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::toggleSceneViewMode()
{
    setSceneViewMode(_sceneCamera.isTwoD()
        ? SceneViewMode::ThreeD : SceneViewMode::TwoD);
}

void EditorSession::setSceneViewMode(SceneViewMode mode, bool persist)
{
    const bool changed = _sceneCamera.mode() != mode;
    if (changed) {
        if (_transformGizmo.active()) finishTransformGizmoDrag(false);
        _sceneCamera.threeD().endLook();
        _sceneCamera.endTwoDPan();
        _gizmoHoverHandle = EditorGizmoHandle::None;
        _sceneCamera.setMode(mode);
    }
    if (mode == SceneViewMode::TwoD && !_twoDSceneViewInitialized) {
        fitTwoDViewToSceneCamera();
        _twoDSceneViewInitialized = true;
    }
    if (changed && persist) rememberCurrentSceneView();
    syncSceneViewToolbar();
    syncSceneVisibilityMenu();
    pushSceneCameraToRenderer();
    syncTransformGizmoToRenderer();
    if (_hasLastMouse) {
        updateViewportCoordinateFeedback(_lastMouseX, _lastMouseY);
    }
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::syncSceneViewToolbar()
{
    const bool twoD = _sceneCamera.isTwoD();
    const bool orthographic =
        _sceneCamera.threeDProjection() == ProjectionMode::Orthographic;

    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_mode"))) {
        const std::wstring label = twoD
            ? localizedText("ui.editor.viewport.view_2d", "2D Scene View")
            : localizedText("ui.editor.viewport.view_3d", "3D Scene View");
        button->setAccessibilityLabel(label);
        if (button->getIconDocument() != nullptr && !_engineAssetsRoot.empty()) {
            const std::filesystem::path iconPath =
                std::filesystem::path(_engineAssetsRoot) / "Icons" / "Tabler"
                / "outline" / (twoD ? "rectangle.svg" : "box.svg");
            std::string error;
            auto icon = ayt::ui::SvgDocument::loadFromFile(iconPath, &error);
            if (icon != nullptr) {
                button->setText(L"");
                button->setIconDocument(std::move(icon));
            } else {
                button->setIconDocument({});
                button->setText(twoD ? L"2D" : L"3D");
            }
        } else {
            button->setText(twoD ? L"2D" : L"3D");
        }
    }
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_camera"))) {
        const std::wstring label = orthographic
            ? localizedText("ui.editor.viewport.orthographic", "Orthographic")
            : localizedText("ui.editor.viewport.perspective", "Perspective");
        button->setText(label);
        button->setAccessibilityLabel(twoD
            ? L"2D Scene View always uses orthographic projection"
            : label + L" projection");
        button->setEnabled(!twoD);
    }
    if (_sceneViewModeMenuItem != nullptr) {
        _sceneViewModeMenuItem->setText(localizedText(
            "ui.editor.viewport.menu_2d", "2D Scene View"));
        setEditorMenuToggleIcon(_sceneViewModeMenuItem, _engineAssetsRoot, twoD);
    }
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_projection"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_projection", "Orthographic Projection"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot, orthographic);
        item->setEnabled(!twoD);
    }
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_shading"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_wireframe", "Wireframe Rendering"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot, _wireframeView);
    }
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_orientation_axis"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_orientation_axis", "Orientation Axis"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot,
                                _viewportOrientationAxisVisible);
    }
}

void EditorSession::setSceneVisibility(
    const EditorSceneVisibility& visibility,
    bool persist)
{
    _sceneVisibility = visibility;
    if (persist) rememberCurrentSceneView();
    syncSceneVisibilityMenu();
    pushSceneCameraToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::syncSceneVisibilityMenu()
{
    const bool editMode = _gameView.mode() == EditorMode::Edit;
    const ayt::math::FVector4 active = editorThemeColor(
        "color.accent", ayt::math::FVector4(0.16f, 0.40f, 0.70f, 1.0f));
    const ayt::math::FVector4 inactive = editorThemeColor(
        "color.text.muted", ayt::math::FVector4(0.68f, 0.71f, 0.76f, 1.0f));
    const auto sync = [this, editMode, &active, &inactive](
        const char* id, const wchar_t* label, bool visible) {
        if (auto* button = dynamic_cast<ayt::ui::Button*>(_ui.findById(id))) {
            button->setIconColor(visible ? active : inactive);
            button->setAccessibilityLabel(
                std::wstring(label) + (visible ? L": visible" : L": hidden"));
            button->setEnabled(editMode);
        }
    };
    sync("btn_view_meshes", L"3D meshes", _sceneVisibility.meshes);
    sync("btn_view_world_lit_2d", L"World-lit 2D content",
         _sceneVisibility.worldLit2D);
    sync("btn_view_camera_overlay_2d", L"Camera-overlay 2D content",
         _sceneVisibility.cameraOverlay2D);
    sync("btn_view_ui", L"UI preview", _sceneVisibility.ui);
    if (_sceneUiPreviewHost != nullptr) {
        _sceneUiPreviewHost->setVisible(editMode && _sceneVisibility.ui);
    }
}

void EditorSession::selectInitialSceneViewForDocument()
{
    if (_document == nullptr) return;
    _twoDSceneViewInitialized = false;
    _sceneVisibility = {};
    if (const EditorSceneVisibility* savedVisibility =
            _sceneViewWorkspace.findVisibility(_document->path())) {
        _sceneVisibility = *savedVisibility;
    }
    const EditorSceneCameraState* saved =
        _sceneViewWorkspace.find(_document->path());
    const char* source = "engine-profile";
    if (saved != nullptr) {
        _sceneCamera.restoreState(*saved);
        _twoDSceneViewInitialized = true;
        source = "workspace";
    } else {
        EditorSceneContentProfile content;
        ayt::entity::World& world = _document->scene().world();
        for (ayt::entity::Entity* entity : world.getAllEntities()) {
            if (entity == nullptr) continue;
            content.hasOrthographicCamera |=
                entity->getComponent<ayt::entity::OrthoCameraComponent>() != nullptr;
            content.hasTwoDContent |=
                entity->getComponent<ayt::entity::SpriteComponent>() != nullptr
                || entity->getComponent<ayt::entity::TilemapComponent>() != nullptr;
            content.hasThreeDContent |=
                entity->getComponent<ayt::entity::MeshComponent>() != nullptr;
        }
        const SceneViewMode mode = chooseInitialSceneViewMode(
            _projectDefaultSceneView, content, _projectEngineProfile);
        if (_projectDefaultSceneView == "2D"
            || _projectDefaultSceneView == "3D") {
            source = "project";
        } else if (content.hasOrthographicCamera || content.hasTwoDContent
                   || content.hasThreeDContent) {
            source = "content";
        }
        _sceneCamera.setMode(mode);
        if (mode == SceneViewMode::TwoD) {
            fitTwoDViewToSceneCamera();
            _twoDSceneViewInitialized = true;
        }
    }
    const ayt::math::FVector2 center = _sceneCamera.twoDCenter();
    std::fprintf(stderr,
        "[EditorSceneView] mode=%s source=%s center=(%.3f,%.3f) viewHeight=%.3f\n",
        _sceneCamera.isTwoD() ? "2D" : "3D", source,
        center.x, center.y, _sceneCamera.twoDViewHeight());
    syncSceneViewToolbar();
    syncSceneVisibilityMenu();
    pushSceneCameraToRenderer();
    refreshSceneUiPreview();
}

void EditorSession::fitTwoDViewToSceneCamera()
{
    if (_document == nullptr) return;
    ayt::entity::World& world = _document->scene().world();

    bool hasWorldContent = false;
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (ayt::entity::Entity* entity : world.getAllEntities()) {
        if (entity == nullptr) continue;
        auto* transform = entity->getComponent<ayt::entity::Transform>();
        if (transform == nullptr) continue;
        bool worldLit = false;
        if (auto* sprite = entity->getComponent<ayt::entity::SpriteComponent>()) {
            worldLit = sprite->visible && sprite->isWorldLit();
        }
        if (auto* tilemap = entity->getComponent<ayt::entity::TilemapComponent>()) {
            worldLit = worldLit || (tilemap->visible && tilemap->isWorldLit());
        }
        if (!worldLit) continue;
        Editor2DSelectionShape shape;
        if (!editor2DSelectionShape(*entity, _assetDatabase,
                                    _sceneCamera.viewportAspect(), shape)) {
            continue;
        }
        const ayt::math::Float4x4 matrix =
            editor2DShapeMatrix(*transform, shape);
        const ayt::math::FVector2 localCorners[4] = {
            {shape.localMin.x, shape.localMin.y},
            {shape.localMax.x, shape.localMin.y},
            {shape.localMax.x, shape.localMax.y},
            {shape.localMin.x, shape.localMax.y},
        };
        for (const ayt::math::FVector2& corner : localCorners) {
            const ayt::math::FVector3 worldCorner = matrix.transformPoint(
                {corner.x, corner.y, 0.0f});
            minX = std::min(minX, worldCorner.x);
            maxX = std::max(maxX, worldCorner.x);
            minY = std::min(minY, worldCorner.y);
            maxY = std::max(maxY, worldCorner.y);
        }
        hasWorldContent = true;
    }
    if (hasWorldContent) {
        const float aspect = std::max(0.01f, _sceneCamera.viewportAspect());
        const float contentWidth = std::max(1.0f, maxX - minX);
        const float contentHeight = std::max(1.0f, maxY - minY);
        const float viewHeight = std::max(contentHeight,
            contentWidth / aspect) * 1.35f;
        _sceneCamera.setTwoDPose(
            {(minX + maxX) * 0.5f, (minY + maxY) * 0.5f}, viewHeight);
        return;
    }

    const ayt::entity::SelectedOrthoCamera2D selected =
        ayt::entity::selectOrthoCamera2D(world);
    if (!selected) return;
    const float viewHeight = selected.camera->effectiveViewSize(
        _sceneCamera.viewportAspect()) / std::fabs(selected.camera->safeZoom());
    _sceneCamera.setTwoDPose(
        {selected.transform->position.x, selected.transform->position.y},
        viewHeight);
}

void EditorSession::rememberCurrentSceneView()
{
    if (_document == nullptr || _document->path().empty()
        || !_sceneViewWorkspace.enabled()) {
        return;
    }
    _sceneViewWorkspace.set(_document->path(), _sceneCamera.state());
    _sceneViewWorkspace.setVisibility(
        _document->path(), _sceneVisibility);
    _sceneViewWorkspaceDirty = true;
    _sceneViewWorkspaceSaveCountdown = 0.5f;
}

void EditorSession::pollSceneViewWorkspace(float dtSeconds)
{
    if (!_sceneViewWorkspaceDirty) return;
    // RMB free-look can run for minutes and continuously changes the camera.
    // Keep persistence outside active editor gestures and use mouse-up as the
    // commit boundary; Windows Error Reporting placed the observed failure in
    // the JSON lookup that was reached by this autosave path.
    if (_sceneCamera.threeD().isLooking()
        || _sceneCamera.isTwoDPanning()
        || _transformGizmo.active()) {
        _sceneViewWorkspaceSaveCountdown = 0.5f;
        return;
    }
    _sceneViewWorkspaceSaveCountdown -= std::max(0.0f, dtSeconds);
    if (_sceneViewWorkspaceSaveCountdown > 0.0f) return;
    if (_document != nullptr && !_document->path().empty()) {
        _sceneViewWorkspace.set(_document->path(), _sceneCamera.state());
        _sceneViewWorkspace.setVisibility(
            _document->path(), _sceneVisibility);
    }
    std::string error;
    if (!_sceneViewWorkspace.save(&error)) {
        std::fprintf(stderr, "[EditorSceneView] workspace save failed: %s\n",
                     error.c_str());
        _sceneViewWorkspaceSaveCountdown = 2.0f;
        return;
    }
    _sceneViewWorkspaceDirty = false;
}

void EditorSession::clearSceneUiPreview()
{
    if (_sceneUiPreviewHost != nullptr) {
        _sceneUiPreviewHost->detachFromParent();
        ayt::ui::destroyWidgetTree(_sceneUiPreviewHost);
        _sceneUiPreviewHost = nullptr;
    }
    // The preview has its own loader because the loader registry contains
    // non-owning Widget pointers. Releasing it after the tree prevents a
    // temporary project HUD from polluting the editor chrome registry.
    _sceneUiPreviewLoader.reset();
}

void EditorSession::refreshSceneUiPreview()
{
    clearSceneUiPreview();
    if (_document == nullptr || _document->path().empty()
        || _projectRoot.empty()) {
        return;
    }

    std::string error;
    const EditorProjectDescriptor descriptor =
        EditorProjectDescriptor::load(_projectRoot, &error);
    if (!descriptor) return;

    std::filesystem::path documentPath;
    try {
        documentPath = std::filesystem::absolute(_document->path())
            .lexically_normal();
    } catch (...) {
        return;
    }
    const std::filesystem::path assetRoot =
        (std::filesystem::path(_projectRoot) / descriptor.assetRoot)
            .lexically_normal();
    const EditorProjectWorldDescriptor* selectedWorld = nullptr;
    for (const EditorProjectWorldDescriptor& world : descriptor.worlds) {
        const std::filesystem::path candidate =
            std::filesystem::absolute(assetRoot / world.scene)
                .lexically_normal();
        if (candidate == documentPath) {
            selectedWorld = &world;
            break;
        }
    }
    if (selectedWorld == nullptr || selectedWorld->ui.empty()) return;

    const std::filesystem::path uiPath = assetRoot / selectedWorld->ui;
    _sceneUiPreviewLoader = std::make_unique<ayt::ui::UILayoutLoader>();
    ayt::ui::Widget* hud =
        _sceneUiPreviewLoader->loadFromFile(uiPath.string());
    if (hud == nullptr) {
        std::fprintf(stderr,
            "[EditorSceneView] UI preview load failed: %s\n",
            uiPath.string().c_str());
        _sceneUiPreviewLoader.reset();
        return;
    }
    auto* host = new PassiveSceneUiPreviewHost();
    host->setId("scene_ui_preview_host");
    host->addChild(hud);
    _ui.getOverlayRoot()->addChild(host);
    _sceneUiPreviewHost = host;
    syncSceneUiPreviewBounds();
    syncSceneVisibilityMenu();
}

void EditorSession::syncSceneUiPreviewBounds()
{
    if (_sceneUiPreviewHost == nullptr) return;
    ayt::math::FRectangle bounds{};
    if (!getViewportBounds(bounds)) return;
    _sceneUiPreviewHost->setPosition({bounds.minX, bounds.minY});
    _sceneUiPreviewHost->setSize({bounds.width(), bounds.height()});
    if (!_sceneUiPreviewHost->getChildren().empty()) {
        ayt::ui::Widget* hud = _sceneUiPreviewHost->getChildren().front();
        hud->setPosition({0.0f, 0.0f});
        hud->setSize({bounds.width(), bounds.height()});
        hud->performLayout();
    }
}

void EditorSession::updateViewportCoordinateFeedback(float x, float y)
{
    auto* label = dynamic_cast<ayt::ui::TextLabel*>(
        _ui.findById("lbl_viewport_coordinates"));
    if (label == nullptr) return;
    if (!_sceneCamera.isTwoD() || !isViewportSurfacePoint(x, y)) {
        label->setText(L"");
        return;
    }
    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)) return;
    const ayt::math::FVector2 logical = _ui.physicalToLogical({x, y});
    const ayt::math::FVector2 world = _sceneCamera.twoDWorldAt(
        {logical.x - viewport.minX, logical.y - viewport.minY});
    wchar_t text[128]{};
    std::swprintf(text, std::size(text), L"XY  X %.1f  Y %.1f  Grid %.3g",
                  world.x, world.y, _sceneCamera.adaptiveGridSpacing());
    label->setText(text);
}

void EditorSession::toggleViewportProjection()
{
    if (_sceneCamera.isTwoD()) return;
    _sceneCamera.setThreeDProjection(
        _sceneCamera.threeDProjection() == ProjectionMode::Perspective
            ? ProjectionMode::Orthographic : ProjectionMode::Perspective);
    rememberCurrentSceneView();
    syncSceneViewToolbar();
    pushSceneCameraToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::toggleViewportShading()
{
    _wireframeView = !_wireframeView;
    const std::wstring label = _wireframeView
        ? localizedText("ui.editor.viewport.wireframe", "Wireframe")
        : localizedText("ui.editor.viewport.shaded", "Shaded");
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_view_shading"))) {
        button->setText(label);
        button->setAccessibilityLabel(label + L" rendering");
    }
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_shading"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_wireframe", "Wireframe Rendering"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot, _wireframeView);
    }
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        sub->renderer().setWireframeEnabled(_wireframeView);
    }
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::setViewportOrientationAxisVisible(bool visible)
{
    const bool changed = _viewportOrientationAxisVisible != visible;
    _viewportOrientationAxisVisible = visible;
    if (_viewportOrientationAxisMenuItem != nullptr) {
        _viewportOrientationAxisMenuItem->setText(localizedText(
            "ui.editor.menu.view.orientation_axis", "Viewport Orientation Axis"));
        setEditorMenuToggleIcon(_viewportOrientationAxisMenuItem,
                                _engineAssetsRoot, visible);
    }
    if (auto* item = dynamic_cast<ayt::ui::MenuItem*>(
            _ui.findById("menu_view_orientation_axis"))) {
        item->setText(localizedText(
            "ui.editor.viewport.menu_orientation_axis", "Orientation Axis"));
        setEditorMenuToggleIcon(item, _engineAssetsRoot, visible);
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

bool EditorSession::newSceneFromTemplate(EditorSceneTemplate sceneTemplate)
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) {
        return false;
    }
    if (_document->isDirty()) {
        const int choice = ::MessageBoxW(
            static_cast<HWND>(_hostWindow),
            L"The current scene has unsaved changes.\n\nDiscard them and create a new scene?",
            L"AYEditor", MB_YESNO | MB_ICONWARNING);
        if (choice != IDYES) return false;
    }
    rememberCurrentSceneView();
    _document->newScene();
    afterDocumentReload();
    if (sceneTemplate == EditorSceneTemplate::TwoD) {
        _sceneCamera.setTwoDPose({0.0f, 0.0f}, 600.0f);
        setSceneViewMode(SceneViewMode::TwoD);
        (void)createTwoDEntity(Editor2DEntityKind::Camera);
    } else {
        setSceneViewMode(SceneViewMode::ThreeD);
    }
    return true;
}

void EditorSession::openSceneDocument()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    if (_document->isDirty()) {
        const int choice = ::MessageBoxW(
            static_cast<HWND>(_hostWindow),
            L"The current scene has unsaved changes.\n\nDiscard them and open another scene?",
            L"AYEditor", MB_YESNO | MB_ICONWARNING);
        if (choice != IDYES) return;
    }
    const std::string path = showSceneOpenDialog(static_cast<HWND>(_hostWindow),
        (std::filesystem::path(_assetDatabase.projectRoot())
            / "Assets" / "worlds").string());
    if (path.empty()) return;

    rememberCurrentSceneView();
    std::string error;
    if (!_document->open(path, &error)) {
        const std::wstring message(error.begin(), error.end());
        ::MessageBoxW(static_cast<HWND>(_hostWindow), message.c_str(), L"Open Scene Failed",
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
        ::MessageBoxW(static_cast<HWND>(_hostWindow), message.c_str(), L"Save Scene Failed",
                      MB_OK | MB_ICONERROR);
        return;
    }
    refreshUnsavedIndicator();
}

void EditorSession::saveSceneDocumentAs()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    const std::string path = showSceneSaveDialog(static_cast<HWND>(_hostWindow),
        (std::filesystem::path(_assetDatabase.projectRoot())
            / "Assets" / "worlds").string());
    if (path.empty()) return;
    std::string error;
    if (!_document->saveAs(path, &error)) {
        const std::wstring message(error.begin(), error.end());
        ::MessageBoxW(static_cast<HWND>(_hostWindow), message.c_str(), L"Save Scene Failed",
                      MB_OK | MB_ICONERROR);
        return;
    }
    refreshOutliner();
    refreshUnsavedIndicator();
    rememberCurrentSceneView();
}

IEditorCommandTarget* EditorSession::activeDocumentCommandTarget() const noexcept
{
    if (_workspace == nullptr) return _document.get();
    IEditorCommandTarget* target = _workspace->commands().activeTarget();
    return target != nullptr ? target : _document.get();
}

bool EditorSession::canExecuteDocumentCommand(
    const std::string& commandId) const
{
    if (_gameView.mode() != EditorMode::Edit) return false;
    IEditorCommandTarget* target = activeDocumentCommandTarget();
    if (target != nullptr && target->handlesCommand(commandId)) {
        return target->canExecuteCommand(commandId);
    }
    if (target != _document.get() || _document == nullptr) return false;
    if (commandId == "file.save") return _document->isDirty();
    if (commandId == "file.save_as") return _document->canSaveAs();
    return false;
}

bool EditorSession::executeDocumentCommand(const std::string& commandId)
{
    if (!canExecuteDocumentCommand(commandId)) return false;
    IEditorCommandTarget* target = activeDocumentCommandTarget();
    if (target != nullptr && target->handlesCommand(commandId)) {
        if (_workspace == nullptr) return false;
        _workspace->commands().setActiveTarget(target);
        return _workspace->commands().execute(commandId);
    }
    if (target != _document.get()) return false;
    if (commandId == "file.save") {
        saveSceneDocument();
        return true;
    }
    if (commandId == "file.save_as") {
        saveSceneDocumentAs();
        return true;
    }
    return false;
}

void EditorSession::syncDocumentCommandMenu()
{
    if (_saveMenuItem != nullptr) {
        _saveMenuItem->setEnabled(canExecuteDocumentCommand("file.save"));
    }
    if (_saveAsMenuItem != nullptr) {
        _saveAsMenuItem->setEnabled(
            canExecuteDocumentCommand("file.save_as"));
    }
    if (_undoMenuItem != nullptr) {
        _undoMenuItem->setEnabled(canExecuteDocumentCommand("edit.undo"));
    }
    if (_redoMenuItem != nullptr) {
        _redoMenuItem->setEnabled(canExecuteDocumentCommand("edit.redo"));
    }
}

void EditorSession::afterDocumentReload()
{
    clearSelectedEntity(false);
    _playRuntime.forgetEditScenePreview();
    refreshOutliner();
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    selectInitialSceneViewForDocument();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::createEmptyEntity()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    ayt::entity::World* world = hierarchyWorldMutable();
    if (world == nullptr) return;
    const std::string name = "Entity "
        + std::to_string(static_cast<unsigned>(
            world->getAllEntities().size() + 1u));
    uint32_t entityId = 0;
    if (!_document->createEntity(
            "Create Entity",
            [&name](ayt::entity::Entity& candidate) {
                candidate.setName(name.c_str());
                return candidate.addComponent<ayt::entity::Transform>()
                    != nullptr;
            },
            &entityId)) {
        return;
    }
    ayt::entity::Entity* entity = world->findEntity(entityId);
    if (entity == nullptr) return;
    setSelectedEntity(world, entity);
    _outlinerRefreshPending = true;
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    if (_repaintCallback) _repaintCallback();
}

uint32_t EditorSession::createTwoDEntity(Editor2DEntityKind kind)
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return 0u;
    ayt::entity::World* world = hierarchyWorldMutable();
    if (world == nullptr) return 0u;
    if (!_sceneCamera.isTwoD()) setSceneViewMode(SceneViewMode::TwoD);

    const ayt::math::FVector2 center = _sceneCamera.twoDCenter();
    const char* typeName = "Sprite";
    const char* inspectedTypeName = "SpriteComponent";
    if (kind == Editor2DEntityKind::Tilemap) {
        typeName = "Tilemap";
        inspectedTypeName = "TilemapComponent";
    } else if (kind == Editor2DEntityKind::Camera) {
        typeName = "2D Camera";
        inspectedTypeName = "OrthoCameraComponent";
    }
    uint32_t entityId = 0;
    if (!_document->createEntity(
            std::string("Create ") + typeName,
            [&, typeName](ayt::entity::Entity& candidate) {
                auto* transform =
                    candidate.addComponent<ayt::entity::Transform>();
                if (transform == nullptr) return false;
                transform->setPosition(center.x, center.y, 0.0f);
                if (kind == Editor2DEntityKind::Sprite) {
                    if (candidate.addComponent<ayt::entity::SpriteComponent>()
                        == nullptr) return false;
                    transform->setScale(100.0f, 100.0f, 1.0f);
                } else if (kind == Editor2DEntityKind::Tilemap) {
                    if (candidate.addComponent<ayt::entity::TilemapComponent>()
                        == nullptr) return false;
                    transform->setPosition(
                        center.x - 16.0f, center.y - 16.0f, 0.0f);
                } else {
                    auto* camera = candidate.addComponent<
                        ayt::entity::OrthoCameraComponent>();
                    if (camera == nullptr) return false;
                    camera->viewSize =
                        std::max(_sceneCamera.twoDViewHeight(), 1.0f);
                    camera->zoom = 1.0f;
                    camera->designWidth = camera->viewSize * 1.6f;
                    camera->designHeight = camera->viewSize;
                    camera->viewportAspect = 1.6f;
                    camera->active = true;
                }
                candidate.setName((std::string(typeName) + " "
                    + std::to_string(static_cast<unsigned>(candidate.getId())))
                    .c_str());
                return true;
            },
            &entityId)) {
        return 0u;
    }
    ayt::entity::Entity* entity = world->findEntity(entityId);
    if (entity == nullptr) return 0u;

    _inspectedComponentTypeName = inspectedTypeName;
    setSelectedEntity(world, entity);
    _outlinerRefreshPending = true;
    refreshInspectorLabels();
    refreshTransformInspector();
    refreshUnsavedIndicator();
    if (_repaintCallback) _repaintCallback();
    return entityId;
}

void EditorSession::deleteSelectedEntity()
{
    if (_document == nullptr || _gameView.mode() != EditorMode::Edit) return;
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    if (world == nullptr || entity == nullptr) return;
    if (!_document->deleteEntity(entity->getId())) return;
    clearSelectedEntity(false);
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

    const auto addLocalizedMenu = [this, menuBar](
        const char* key, const wchar_t* fallback) {
        const std::size_t index = menuBar->getMenuCount();
        ayt::ui::Menu* menu = menuBar->addMenu(localizedText(key, fallback));
        if (menu != nullptr) {
            // Runtime-created menus must retain the same stable identity as
            // menus loaded from UI JSON. Display text changes when the editor
            // language changes and is not a safe lookup key.
            menu->setLocalizationKey("title", key);
        }
        gEditorMenuTexts[this].push_back(
            {nullptr, menuBar, index, key, fallback});
        return menu;
    };
    const auto addLocalizedItem = [this](ayt::ui::Menu* menu,
                                          const char* key,
                                          const wchar_t* fallback) {
        if (menu == nullptr) return static_cast<ayt::ui::MenuItem*>(nullptr);
        ayt::ui::MenuItem* item = menu->addItem(localizedText(key, fallback));
        if (item != nullptr) {
            item->setLocalizationKey("text", key);
            gEditorMenuTexts[this].push_back(
                {item, nullptr, 0u, key, fallback});
        }
        return item;
    };

    ayt::ui::Menu* fileMenu = addLocalizedMenu("ui.editor.menu.file._label", L"File");
    if (fileMenu != nullptr) {
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.new_empty_scene", L"New Empty Scene")) {
            item->setOnActivate([this]() {
                (void)newSceneFromTemplate(EditorSceneTemplate::Empty);
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.new_2d_scene", L"New 2D Scene")) {
            item->setOnActivate([this]() {
                (void)newSceneFromTemplate(EditorSceneTemplate::TwoD);
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.new_ui_layout", L"New UI Layout")) {
            item->setOnActivate([this]() {
                (void)createProjectAsset(EditorAssetType::UiLayout);
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.new_ui_flow", L"New UI Flow")) {
            item->setOnActivate([this]() {
                (void)createProjectAsset(EditorAssetType::UiFlow);
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.new_game_flow", L"New Game Flow")) {
            item->setOnActivate([this]() {
                (void)createProjectAsset(EditorAssetType::GameFlow);
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.new_tilemap", L"New Tilemap")) {
            item->setOnActivate([this]() {
                (void)createProjectAsset(EditorAssetType::Tilemap);
            });
        }
        fileMenu->addSeparator();
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.open_scene", L"Open Scene...")) {
            item->setOnActivate([this]() { openSceneDocument(); });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.save", L"Save")) {
            _saveMenuItem = item;
            item->setShortcut(
                EditorShortcutRegistry::instance().shortcutFor("file.save"));
            item->setOnActivate([this]() {
                (void)executeDocumentCommand("file.save");
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.save_as", L"Save As...")) {
            _saveAsMenuItem = item;
            item->setOnActivate([this]() {
                (void)executeDocumentCommand("file.save_as");
            });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.autosave", L"Autosave Now")) {
            item->setOnActivate([this]() { (void)autosaveNow(); });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.restore_recovery", L"Restore Crash Recovery")) {
            _restoreRecoveryMenuItem = item;
            item->setEnabled(hasCrashRecovery());
            item->setOnActivate([this]() { showCrashRecoveryDialog(); });
        }
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.import", L"Import...")) {
            item->setOnActivate([this]() { importCharacterFromDialog(); });
        }
        fileMenu->addSeparator();
        if (auto* item = addLocalizedItem(fileMenu, "ui.editor.menu.file.exit", L"Exit")) {
            item->setOnActivate([this]() { requestHostClose(); });
        }
    }

    ayt::ui::Menu* editMenu = addLocalizedMenu("ui.editor.menu.edit._label", L"Edit");
    if (editMenu != nullptr) {
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.undo", L"Undo")) {
            _undoMenuItem = item;
            item->setShortcut(
                EditorShortcutRegistry::instance().shortcutFor("edit.undo"));
            item->setOnActivate([this]() {
                (void)executeDocumentCommand("edit.undo");
            });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.redo", L"Redo")) {
            _redoMenuItem = item;
            item->setShortcut(
                EditorShortcutRegistry::instance().shortcutFor("edit.redo"));
            item->setOnActivate([this]() {
                (void)executeDocumentCommand("edit.redo");
            });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.restore_deleted", L"Restore Last Deleted Assets")) {
            _restoreDeletedMenuItem = item;
            item->setEnabled(_assetTrash != nullptr
                && _assetTrash->canRestoreLast());
            item->setOnActivate([this]() {
                (void)restoreLastDeletedAssets();
            });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.project_trash", L"Project Trash...")) {
            item->setOnActivate([this]() { showAssetTrashDialog(); });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.operation_history", L"Resource Operation History...")) {
            item->setOnActivate([this]() { showAssetHistoryDialog(); });
        }
        editMenu->addSeparator();
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.create_entity", L"Create Empty Entity")) {
            item->setOnActivate([this]() { createEmptyEntity(); });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.create_sprite", L"Create Sprite")) {
            item->setOnActivate([this]() {
                (void)createTwoDEntity(Editor2DEntityKind::Sprite);
            });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.create_tilemap", L"Create Tilemap")) {
            item->setOnActivate([this]() {
                (void)createTwoDEntity(Editor2DEntityKind::Tilemap);
            });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.create_2d_camera", L"Create 2D Camera")) {
            item->setOnActivate([this]() {
                (void)createTwoDEntity(Editor2DEntityKind::Camera);
            });
        }
        if (auto* item = addLocalizedItem(editMenu, "ui.editor.menu.edit.delete_selected", L"Delete Selected")) {
            item->setShortcut(
                EditorShortcutRegistry::instance().shortcutFor("edit.delete"));
            item->setOnActivate([this]() { deleteSelectedEntity(); });
        }
    }

    ayt::ui::Menu* viewMenu = addLocalizedMenu("ui.editor.menu.view._label", L"View");
    if (viewMenu != nullptr) {
        if (auto* item = addLocalizedItem(viewMenu, "ui.editor.menu.view.orientation_axis", L"Viewport Orientation Axis")) {
            _viewportOrientationAxisMenuItem = item;
            item->setOnActivate([this]() {
                setViewportOrientationAxisVisible(
                    !_viewportOrientationAxisVisible);
            });
            item->setText(localizedText(
                "ui.editor.menu.view.orientation_axis",
                "Viewport Orientation Axis"));
            setEditorMenuToggleIcon(item, _engineAssetsRoot,
                                    _viewportOrientationAxisVisible);
        }
    }

    ayt::ui::Menu* windowMenu = addLocalizedMenu("ui.editor.menu.window._label", L"Window");
    if (windowMenu != nullptr) {
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.render_settings", L"Render Settings")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_render", _panelRenderVisible);
            });
        }
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.inspector", L"Inspector")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_inspector", _panelInspectorVisible);
            });
        }
        // v0.3+ PR-5 — Hierarchy panel toggle (design §4.3.y)
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.hierarchy", L"Hierarchy")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_outliner", _panelOutlinerVisible);
            });
        }
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.network", L"Network")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_network", _panelNetworkVisible);
            });
        }
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.console", L"Console")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_console", _panelConsoleVisible);
            });
        }
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.assets", L"Assets")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_assets", _panelAssetsVisible);
            });
        }
        windowMenu->addSeparator();
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.save_workspace", L"Save Workspace")) {
            item->setOnActivate([this]() { savePreferencesNow(); });
        }
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.reset_workspace", L"Reset Workspace Layout")) {
            item->setOnActivate([this]() { resetWorkspacePreferences(); });
        }
        windowMenu->addSeparator();
        if (auto* item = addLocalizedItem(windowMenu, "ui.editor.menu.window.select_character", L"Select Character")) {
            item->setOnActivate([this]() { selectCharacter(); });
        }
    }

    ayt::ui::Menu* toolsMenu = addLocalizedMenu("ui.editor.menu.tools._label", L"Tools");
    if (toolsMenu != nullptr) {
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.run_project", L"Run Current Project")) {
            item->setOnActivate([this]() { (void)runCurrentProject(); });
        }
        toolsMenu->addSeparator();
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.ui_layout", L"UI Layout Editor...")) {
            item->setOnActivate([this]() { (void)openUiLayoutEditor(); });
        }
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.ui_flow", L"UI Flow Editor...")) {
            item->setOnActivate([this]() { (void)openUiFlowEditor(); });
        }
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.game_flow", L"Game Flow Editor...")) {
            item->setOnActivate([this]() { (void)openGameFlowEditor(); });
        }
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.tilemap", L"2D Tilemap Editor...")) {
            item->setId("menu_tools_tilemap_editor");
            item->setOnActivate([this]() { (void)openTilemapEditor(); });
        }
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.audio", L"Audio Editor...")) {
            item->setOnActivate([this]() {
                (void)openRegisteredTool(kEditorAudioToolExtensionId);
            });
        }
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.timeline", L"Timeline")) {
            item->setOnActivate([this]() {
                (void)openRegisteredTool(kEditorTimelineToolExtensionId);
            });
        }
        toolsMenu->addSeparator();
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.reimport_changed", L"Reimport Changed Source Assets")) {
            item->setOnActivate([this]() {
                if (_assetImportQueue == nullptr) return;
                const std::size_t count =
                    _assetImportQueue->enqueueChangedDependencies(_assetDatabase);
                setAssetBrowserStatus(count == 0u
                    ? L"No changed source assets require reimport."
                    : L"Queued " + std::to_wstring(count)
                        + L" changed source asset(s).", true);
            });
        }
        if (auto* item = addLocalizedItem(toolsMenu, "ui.editor.menu.tools.validate_project", L"Validate Project Content")) {
            item->setOnActivate([this]() { validateProjectContent(); });
        }
    }

    ayt::ui::Menu* helpMenu = addLocalizedMenu("ui.editor.menu.help._label", L"Help");
    if (helpMenu != nullptr) {
        if (auto* item = addLocalizedItem(helpMenu, "ui.editor.menu.help.about", L"About AYEditor")) {
            item->setOnActivate([]() {});
        }
    }

    for (ayt::ui::Widget* child : menuBar->getChildren()) {
        if (auto* anchor = dynamic_cast<ayt::ui::Button*>(child)) {
            anchor->setStyleId("editor_menu_anchor");
            anchor->setPadding(8.0f, 1.0f, 8.0f, 1.0f);
        }
    }
    syncDocumentCommandMenu();
}

bool EditorSession::openUiLayoutEditor(const std::string& path) {
    const auto openStarted = std::chrono::steady_clock::now();
    if (_childWindows == nullptr || _workspace == nullptr) {
        setAssetBrowserStatus(
            L"UI Designer requires the AYDevice child-window host", true);
        return false;
    }

    syncUiDesignerLifetime();
    if (_uiDesigner != nullptr && _uiDesignerHandle != nullptr) {
        const bool sameDocument = path.empty()
            || EditorDocumentManager::normalizeResourceKey(path)
                == EditorDocumentManager::normalizeResourceKey(
                    _uiDesignerDocument != nullptr
                        ? _uiDesignerDocument->path() : std::string{});
        if (sameDocument) {
            (void)_workspace->documents().activate(_uiDesignerDocumentId);
            (void)_childWindows->activateChildWindow(_uiDesignerHandle);
            setAssetBrowserStatus(L"UI Designer focused");
            return true;
        }
        if (!confirmUiDesignerClose()) return false;
        const EditorChildWindowManager::Handle previousHandle =
            _uiDesignerHandle;
        // confirmUiDesignerClose() has already removed the document from the
        // workspace. Release the controller before closing the native window
        // so its close callback does not try to close the same document again.
        releaseUiDesigner(false);
        (void)_childWindows->closeChildWindow(previousHandle);
    }

    EditorOpenRequest request;
    request.resourcePath = path;
    request.resourceKey = path.empty()
        ? "workspace:ui-layout:untitled" : path;
    request.displayPath = path.empty() ? "Untitled UI Layout" : path;
    request.preferredEditorId = kEditorUiLayoutExtensionId;

    EditorOpenResult opened = _workspace->documents().open(request);
    if (!opened) {
        setAssetBrowserStatus(
            L"UI Designer open failed: "
            + ayt::ui::decodeUtf8Text(opened.error), true);
        return false;
    }
    auto document = std::dynamic_pointer_cast<EditorUiLayoutDocument>(
        opened.document);
    if (document == nullptr) {
        if (opened.status == EditorOpenStatus::Opened) {
            (void)_workspace->documents().close(
                opened.documentId, EditorDocumentCloseAction::Discard);
        }
        setAssetBrowserStatus(L"UI Designer document type mismatch", true);
        return false;
    }

    _uiDesignerDocumentId = opened.documentId;
    _uiDesignerDocument = std::move(document);
    const auto documentReady = std::chrono::steady_clock::now();

    EditorUiLayoutExtensionConfig controllerConfig;
    controllerConfig.openPathPicker = [this]() {
        HWND owner = _uiDesignerHandle != nullptr
            ? static_cast<HWND>(_uiDesignerHandle) : static_cast<HWND>(_hostWindow);
        return showUiJsonOpenDialog(owner,
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    controllerConfig.savePathPicker = [this]() {
        HWND owner = _uiDesignerHandle != nullptr
            ? static_cast<HWND>(_uiDesignerHandle) : static_cast<HWND>(_hostWindow);
        return showUiJsonSaveDialog(owner,
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    controllerConfig.texturePathPicker = [this]() {
        HWND owner = _uiDesignerHandle != nullptr
            ? static_cast<HWND>(_uiDesignerHandle) : static_cast<HWND>(_hostWindow);
        return showUiTextureOpenDialog(owner,
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "textures").string());
    };
    controllerConfig.themePathPicker = [this]() {
        HWND owner = _uiDesignerHandle != nullptr
            ? static_cast<HWND>(_uiDesignerHandle) : static_cast<HWND>(_hostWindow);
        return showUiJsonOpenDialog(owner,
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui" / "themes").string());
    };
    controllerConfig.textureResourceProvider = [this]() {
        return enumerateUiTextureResources(
            _engineAssetsRoot, _assetDatabase.projectRoot());
    };
    controllerConfig.externalComponentLibraryPath = [this]() {
        return (std::filesystem::path(resolveProjectAssetRoot(
                    _assetDatabase.projectRoot()))
                / "ui" / "project.ayuicomponents.json").string();
    };
    controllerConfig.openOwningFlowAction = [this](
        const std::string& layoutPath, std::string& message) {
        return openOwningFlowForLayout(layoutPath, message);
    };
    controllerConfig.completeFlowSignalsAction = [this](
        const std::string& layoutPath, std::string& message) {
        return completeFlowSignalsForLayout(layoutPath, message);
    };
    controllerConfig.projectRefactorKinds = uiProjectRefactorKinds();
    controllerConfig.projectRefactorAction = [this](
        const std::string& layoutPath, const std::string& kind,
        const std::string& oldValue, const std::string& newValue, bool apply) {
        return refactorUiProjectReferences(
            layoutPath, kind, oldValue, newValue, apply);
    };
    _uiDesigner = std::make_unique<EditorUiLayoutController>(
        _uiDesignerDocument, std::move(controllerConfig));
    _uiDesigner->setStateChanged([this]() {
        refreshUiDesignerTitle();
    });
    const auto controllerReady = std::chrono::steady_clock::now();

    ChildWindowConfig cfg;
    cfg.title = "AYUI Designer";
    cfg.layoutPath = resolveLayoutEditorChromePath(_engineAssetsRoot);
    cfg.x = 96;
    cfg.y = 72;
    cfg.width = 1360;
    cfg.height = 820;
    cfg.showOnOpen = false;
    cfg.beforeMouseButton = [this](
        ayt::ui::UIManager& ui, float x, float y,
        int button, bool pressed) {
        if (_uiDesigner == nullptr) return false;
        const ayt::math::FVector2 logical = ui.physicalToLogical({x, y});
        return pressed
            ? _uiDesigner->onPointerDown(logical.x, logical.y, button)
            : _uiDesigner->onPointerUp(logical.x, logical.y, button);
    };
    cfg.beforeMouseMove = [this](
        ayt::ui::UIManager& ui, float x, float y) {
        if (_uiDesigner == nullptr) return false;
        const ayt::math::FVector2 logical = ui.physicalToLogical({x, y});
        return _uiDesigner->onPointerMove(logical.x, logical.y);
    };
    cfg.beforeMouseWheel = [this](
        ayt::ui::UIManager& ui, float x, float y, float deltaY) {
        if (_uiDesigner == nullptr) return false;
        const ayt::math::FVector2 logical = ui.physicalToLogical({x, y});
        return _uiDesigner->onWheel(logical.x, logical.y, deltaY);
    };
    cfg.beforeKey = [this](
        ayt::ui::UIManager&, ayt::device::KeyCode key, bool pressed) {
        if (_uiDesigner == nullptr) return false;
        const int uiKey = static_cast<int>(ayt::ui::fromDeviceKey(key));
        if (pressed) return _uiDesigner->onKeyDown(uiKey);
        _uiDesigner->onKeyUp(uiKey);
        return false;
    };
    cfg.onFocusChanged = [this](ayt::ui::UIManager&, bool focused) {
        if (!focused && _uiDesigner != nullptr) {
            _uiDesigner->onKeyUp(ayt::ui::UIKey_Space);
        }
    };
    cfg.beforeCloseRequested = [this](ayt::ui::UIManager&) {
        return confirmUiDesignerClose();
    };
    cfg.beforeClose = [this](ayt::ui::UIManager&) {
        releaseUiDesigner(true);
    };
    cfg.resolveCursorHint = [this](
        ayt::ui::UIManager& ui, float x, float y) {
        if (_uiDesigner == nullptr) {
            return ayt::ui::UiCursorHint::Default;
        }
        const ayt::math::FVector2 logical = ui.physicalToLogical({x, y});
        return _uiDesigner->cursorHint(logical.x, logical.y);
    };

    EditorChildWindowManager::Handle handle = nullptr;
    if (!_childWindows->openChildWindow(cfg, handle) || handle == nullptr) {
        releaseUiDesigner(true);
        setAssetBrowserStatus(L"UI Designer window creation failed", true);
        return false;
    }
    const auto childReady = std::chrono::steady_clock::now();
    _uiDesignerHandle = handle;
    ayt::ui::UIManager* childUi = _childWindows->uiForHandle(handle);
    if (childUi == nullptr || !_uiDesigner->attach(*childUi)) {
        _childWindows->closeChildWindow(handle);
        setAssetBrowserStatus(L"UI Designer UI attach failed", true);
        return false;
    }
    const auto sessionAttached = std::chrono::steady_clock::now();
    if (!_uiDesignerDocument->path().empty()
        && !_uiDesigner->openDocument(_uiDesignerDocument->path())) {
        _childWindows->closeChildWindow(handle);
        setAssetBrowserStatus(L"UI Designer document load failed", true);
        return false;
    }
    const auto documentLoaded = std::chrono::steady_clock::now();

    refreshUiDesignerTitle();
    if (!_childWindows->showChildWindow(handle)) {
        _childWindows->closeChildWindow(handle);
        setAssetBrowserStatus(L"UI Designer window show failed", true);
        return false;
    }
    if (editorUiTimingsEnabled()) {
        std::fprintf(stderr,
            "[EditorUiTiming] designer total_us=%llu document_us=%llu "
            "controller_us=%llu child_us=%llu attach_us=%llu "
            "document_load_us=%llu show_us=%llu path='%s'\n",
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(openStarted)),
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(openStarted, documentReady)),
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(documentReady, controllerReady)),
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(controllerReady, childReady)),
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(childReady, sessionAttached)),
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(sessionAttached, documentLoaded)),
            static_cast<unsigned long long>(
                editorUiElapsedMicroseconds(documentLoaded)),
            path.c_str());
        std::fflush(stderr);
    }
    setAssetBrowserStatus(L"UI Designer opened in a dedicated window");
    return true;
}

void EditorSession::syncUiDesignerLifetime()
{
    if (_uiDesignerHandle == nullptr || _childWindows == nullptr) return;
    for (const auto& entry : _childWindows->entries()) {
        if (entry.handle == _uiDesignerHandle) return;
    }
    releaseUiDesigner(true);
}

bool EditorSession::confirmUiDesignerClose()
{
    if (_uiDesignerDocument == nullptr || _workspace == nullptr) return true;
    EditorDocumentCloseAction action = EditorDocumentCloseAction::Discard;
    if (_uiDesignerDocument->isDirty()) {
        if (static_cast<HWND>(_hostWindow) == nullptr) {
            action = EditorDocumentCloseAction::Discard;
        } else {
            const std::wstring prompt =
                ayt::ui::decodeUtf8Text(_uiDesignerDocument->title())
                + L" has unsaved changes.\n\nSave before closing?";
            HWND owner = _uiDesignerHandle != nullptr
                ? static_cast<HWND>(_uiDesignerHandle) : static_cast<HWND>(_hostWindow);
            const int choice = ::MessageBoxW(
                owner, prompt.c_str(), L"AYUI Designer",
                MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDCANCEL) return false;
            action = choice == IDYES
                ? EditorDocumentCloseAction::Save
                : EditorDocumentCloseAction::Discard;
        }
    }
    const EditorCloseResult closed = _workspace->documents().close(
        _uiDesignerDocumentId, action);
    if (!closed) {
        if (closed.status == EditorCloseStatus::SaveFailed) {
            setAssetBrowserStatus(
                L"UI Designer save failed: "
                + ayt::ui::decodeUtf8Text(closed.error), true);
        }
        return false;
    }
    return true;
}

void EditorSession::releaseUiDesigner(bool closeDocument)
{
    const std::string documentId = _uiDesignerDocumentId;
    if (_uiDesigner != nullptr) _uiDesigner->detach();
    _uiDesigner.reset();
    _uiDesignerDocument.reset();
    _uiDesignerDocumentId.clear();
    _uiDesignerHandle = nullptr;
    if (closeDocument && _workspace != nullptr && !documentId.empty()
        && _workspace->documents().find(documentId) != nullptr) {
        (void)_workspace->documents().close(
            documentId, EditorDocumentCloseAction::Discard);
    }
}

void EditorSession::refreshUiDesignerTitle()
{
    if (_childWindows == nullptr || _uiDesignerHandle == nullptr
        || _uiDesignerDocument == nullptr) {
        return;
    }
    std::string title = "AYUI Designer - " + _uiDesignerDocument->title();
    if (_uiDesignerDocument->isDirty()) title += " *";
    (void)_childWindows->setChildWindowTitle(_uiDesignerHandle, title);
}

EditorUiDesignerWorkflow* EditorSession::uiDesignerProjectWorkflow(
    std::string* error)
{
    const std::string assetRoot = resolveProjectAssetRoot(
        _assetDatabase.projectRoot());
    if (assetRoot.empty()) {
        if (error != nullptr) *error = "Project asset root is unavailable";
        return nullptr;
    }
    if (_uiDesignerWorkflow == nullptr
        || EditorDocumentManager::normalizeResourceKey(
               _uiDesignerWorkflow->assetRoot())
            != EditorDocumentManager::normalizeResourceKey(assetRoot)) {
        auto workflow = std::make_unique<EditorUiDesignerWorkflow>(assetRoot);
        if (!workflow->refresh(error)) return nullptr;
        _uiDesignerWorkflow = std::move(workflow);
    }
    return _uiDesignerWorkflow.get();
}

void EditorSession::pollUiDesignerProjectChanges(float dtSeconds)
{
    if (_uiDesigner == nullptr && _uiFlowDesigner == nullptr
        && _uiDesignerWorkflow == nullptr) {
        return;
    }
    _uiDesignerWorkflowPollCountdown -= std::max(0.0f, dtSeconds);
    if (_uiDesignerWorkflowPollCountdown > 0.0f) return;
    _uiDesignerWorkflowPollCountdown = 0.5f;

    std::string error;
    EditorUiDesignerWorkflow* workflow = uiDesignerProjectWorkflow(&error);
    if (workflow == nullptr) return;
    bool changed = false;
    std::vector<std::string> changedPaths;
    if (!workflow->refreshIfChanged(&changed, &changedPaths, &error)) {
        setAssetBrowserStatus(
            L"UI project index refresh failed: "
            + ayt::ui::decodeUtf8Text(error), true);
        return;
    }
    if (!changed || _workspace == nullptr) return;

    std::unordered_set<std::string> changedKeys;
    for (const std::string& path : changedPaths) {
        changedKeys.insert(EditorDocumentManager::normalizeResourceKey(path));
    }
    std::size_t reloaded = 0u;
    std::size_t conflicts = 0u;
    for (const EditorDocumentRecord& record :
         _workspace->documents().records()) {
        if (record.document == nullptr
            || changedKeys.find(EditorDocumentManager::normalizeResourceKey(
                   record.document->path())) == changedKeys.end()) {
            continue;
        }
        if (record.document->isDirty() || !record.document->canReload()) {
            ++conflicts;
            continue;
        }
        std::string reloadError;
        if (record.document->reload(&reloadError)) ++reloaded;
        else {
            ++conflicts;
            error = std::move(reloadError);
        }
    }
    refreshUiDesignerTitle();
    refreshUiFlowDesignerTitle();
    if (conflicts != 0u) {
        setAssetBrowserStatus(
            L"UI project changed externally; "
            + std::to_wstring(conflicts)
            + L" dirty/missing document(s) need review", true);
    } else if (reloaded != 0u) {
        setAssetBrowserStatus(
            L"UI project index updated; reloaded "
            + std::to_wstring(reloaded) + L" document(s)");
    }
}

bool EditorSession::openOwningFlowForLayout(
    const std::string& layoutPath, std::string& message)
{
    EditorUiDesignerWorkflow* workflow = uiDesignerProjectWorkflow(&message);
    if (workflow == nullptr) return false;
    bool changed = false;
    if (!workflow->refreshIfChanged(&changed, nullptr, &message)) return false;
    const auto links = workflow->screensForLayout(layoutPath);
    if (links.empty()) {
        message = "No UI Flow Screen references this Layout";
        return false;
    }
    const EditorUiScreenLayoutLink& link = links.front();
    if (!openUiFlowEditor(link.flowPath) || _uiFlowDesigner == nullptr
        || !_uiFlowDesigner->selectScreen(link.screenId)) {
        message = "Owning UI Flow Screen could not be opened";
        return false;
    }
    message = "Opened " + link.screenId + " in "
        + std::filesystem::path(link.flowPath).filename().string();
    return true;
}

bool EditorSession::completeFlowSignalsForLayout(
    const std::string& layoutPath, std::string& message)
{
    if (_uiDesignerDocument != nullptr && _uiDesignerDocument->isDirty()) {
        message = "Save the UI Layout before completing Flow Signals";
        return false;
    }
    EditorUiDesignerWorkflow* workflow = uiDesignerProjectWorkflow(&message);
    if (workflow == nullptr) return false;
    bool changed = false;
    if (!workflow->refreshIfChanged(&changed, nullptr, &message)) return false;
    const auto links = workflow->screensForLayout(layoutPath);
    if (links.empty()) {
        message = "No UI Flow Screen references this Layout";
        return false;
    }
    if (_uiFlowDesignerDocument != nullptr
        && _uiFlowDesignerDocument->isDirty()) {
        for (const auto& link : links) {
            if (EditorDocumentManager::normalizeResourceKey(link.flowPath)
                == EditorDocumentManager::normalizeResourceKey(
                    _uiFlowDesignerDocument->path())) {
                message = "Save the open UI Flow before completing Signals";
                return false;
            }
        }
    }
    std::size_t total = 0u;
    for (const auto& link : links) {
        std::size_t applied = 0u;
        if (!workflow->applyHandlerCompletions(
                link.flowPath, link.screenId, {}, &applied, &message)) {
            return false;
        }
        total += applied;
    }
    if (_uiFlowDesignerDocument != nullptr) {
        for (const auto& link : links) {
            if (EditorDocumentManager::normalizeResourceKey(link.flowPath)
                == EditorDocumentManager::normalizeResourceKey(
                    _uiFlowDesignerDocument->path())) {
                if (!_uiFlowDesignerDocument->reload(&message)) return false;
                if (_uiFlowDesigner != nullptr) {
                    (void)_uiFlowDesigner->selectScreen(link.screenId);
                }
                break;
            }
        }
    }
    message = total == 0u
        ? "All Widget handlers already have Flow Signals"
        : "Completed " + std::to_string(total)
            + " Widget handler to Flow Signal binding(s)";
    return true;
}

ayt::ui::LayoutProjectRefactorResult
EditorSession::refactorUiProjectReferences(
    const std::string& layoutPath, const std::string& kind,
    const std::string& oldValue, const std::string& newValue, bool apply)
{
    ayt::ui::LayoutProjectRefactorResult result;
    if (_uiDesignerDocument != nullptr && _uiDesignerDocument->isDirty()) {
        result.message = "Save the UI Layout before project refactoring";
        return result;
    }

    EditorUiRenameRequest request;
    if (kind == "widget-id") {
        request.kind = EditorUiRenameKind::WidgetId;
        request.scopePath = layoutPath;
    } else if (kind == "widget-handler") {
        request.kind = EditorUiRenameKind::WidgetHandler;
        request.scopePath = layoutPath;
    } else if (kind == "flow-signal") {
        request.kind = EditorUiRenameKind::FlowSignal;
    } else if (kind == "layout-reference") {
        request.kind = EditorUiRenameKind::LayoutAsset;
    } else {
        result.message = "Unknown project reference kind";
        return result;
    }

    const std::filesystem::path assetRoot = resolveProjectAssetRoot(
        _assetDatabase.projectRoot());
    auto projectAssetValue = [&assetRoot](const std::string& value) {
        std::filesystem::path path(value);
        if (!path.is_absolute()) return path.generic_string();
        std::error_code error;
        std::filesystem::path relative = std::filesystem::relative(
            path, assetRoot, error);
        return error ? path.generic_string() : relative.generic_string();
    };
    request.oldValue = request.kind == EditorUiRenameKind::LayoutAsset
        ? projectAssetValue(oldValue) : oldValue;
    request.newValue = request.kind == EditorUiRenameKind::LayoutAsset
        ? projectAssetValue(newValue) : newValue;

    std::string error;
    EditorUiDesignerWorkflow* workflow = uiDesignerProjectWorkflow(&error);
    if (workflow == nullptr) {
        result.message = std::move(error);
        return result;
    }
    bool indexChanged = false;
    if (!workflow->refreshIfChanged(&indexChanged, nullptr, &error)) {
        result.message = std::move(error);
        return result;
    }
    const EditorUiRenamePlan plan = workflow->planRename(request);
    result.safe = plan.safe;
    result.changedFiles = plan.edits.size();
    for (const EditorUiFileEdit& edit : plan.edits) {
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(
            edit.path, assetRoot, relativeError);
        const std::string shown = relativeError
            ? edit.path : relative.generic_string();
        result.details.push_back(ayt::ui::decodeUtf8Text(
            shown + " — " + std::to_string(edit.replacementCount)
            + " typed reference(s)"));
    }
    for (const std::string& diagnostic : plan.diagnostics) {
        result.details.push_back(
            L"ERROR — " + ayt::ui::decodeUtf8Text(diagnostic));
    }
    if (!apply) {
        result.succeeded = true;
        result.message = plan.safe
            ? "Safe preview: " + std::to_string(plan.edits.size())
                + " file(s) will change"
            : "Rename preview contains blocking diagnostics";
        return result;
    }
    if (!plan.safe) {
        result.message = "Rename plan is not safe to apply";
        return result;
    }

    if (_workspace != nullptr) {
        for (const EditorUiFileEdit& edit : plan.edits) {
            const std::string editKey =
                EditorDocumentManager::normalizeResourceKey(edit.path);
            for (const EditorDocumentRecord& record :
                 _workspace->documents().records()) {
                if (record.document == nullptr
                    || EditorDocumentManager::normalizeResourceKey(
                           record.document->path()) != editKey) {
                    continue;
                }
                if (record.document->isDirty()) {
                    result.message = "Save dirty document before refactoring: "
                        + record.document->title();
                    return result;
                }
            }
        }
    }

    if (!workflow->applyRename(plan, &error)) {
        result.message = std::move(error);
        return result;
    }
    result.succeeded = true;
    result.message = "Safely renamed typed references in "
        + std::to_string(plan.edits.size()) + " file(s)";

    if (_workspace != nullptr) {
        for (const EditorDocumentRecord& record :
             _workspace->documents().records()) {
            if (record.document == nullptr || !record.document->canReload()) {
                continue;
            }
            const std::string documentKey =
                EditorDocumentManager::normalizeResourceKey(
                    record.document->path());
            const bool changed = std::any_of(
                plan.edits.begin(), plan.edits.end(),
                [&](const EditorUiFileEdit& edit) {
                    return EditorDocumentManager::normalizeResourceKey(edit.path)
                        == documentKey;
                });
            if (changed) {
                std::string reloadError;
                if (!record.document->reload(&reloadError)) {
                    result.details.push_back(
                        L"RELOAD — " + ayt::ui::decodeUtf8Text(reloadError));
                }
            }
        }
    }
    return result;
}

bool EditorSession::openLayoutForFlowScreen(
    const std::string& layoutAsset, std::string& message)
{
    if (layoutAsset.empty()) {
        message = "Selected Screen has no Layout asset";
        return false;
    }
    std::filesystem::path path(layoutAsset);
    if (!path.is_absolute()) {
        path = std::filesystem::path(resolveProjectAssetRoot(
            _assetDatabase.projectRoot())) / path;
    }
    if (!openUiLayoutEditor(path.lexically_normal().string())) {
        message = "UI Layout could not be opened";
        return false;
    }
    message = "Opened UI Layout " + path.filename().string();
    return true;
}

bool EditorSession::openGameFlowEditor(const std::string& requestedPath)
{
    if (_dockViewHost == nullptr || _workspace == nullptr) {
        setAssetBrowserStatus(
            L"Game Flow Editor requires the main editor workspace", true);
        return false;
    }

    std::string path = requestedPath;
    if (path.empty() && !_projectRoot.empty()) {
        const std::filesystem::path descriptorPath =
            std::filesystem::path(_projectRoot)
            / std::string(kEditorProjectDescriptorFile);
        std::error_code descriptorExistsError;
        const bool descriptorPresent = std::filesystem::is_regular_file(
            descriptorPath, descriptorExistsError);
        if (descriptorExistsError) {
            setAssetBrowserStatus(L"Game Flow project descriptor check failed: "
                + ayt::ui::decodeUtf8Text(
                    descriptorExistsError.message()), true);
            return false;
        }
        if (descriptorPresent) {
            std::string descriptorError;
            const EditorProjectDescriptor descriptor =
                EditorProjectDescriptor::load(_projectRoot, &descriptorError);
            if (!descriptor) {
                setAssetBrowserStatus(L"Game Flow project descriptor is invalid: "
                    + ayt::ui::decodeUtf8Text(descriptorError), true);
                return false;
            }
            if (!descriptor.startupFlow.empty()) {
                const std::filesystem::path configured =
                    std::filesystem::path(_projectRoot)
                    / (descriptor.assetRoot.empty() ? "Assets"
                                                    : descriptor.assetRoot)
                    / std::filesystem::path(descriptor.startupFlow);
                std::error_code existsError;
                if (std::filesystem::is_regular_file(configured, existsError)) {
                    path = configured.lexically_normal().string();
                } else {
                    const std::string reason = existsError
                        ? existsError.message() : "file does not exist";
                    setAssetBrowserStatus(
                        L"Configured startupFlow cannot be opened: "
                        + ayt::ui::decodeUtf8Text(
                            configured.lexically_normal().string() + " ("
                            + reason + ")"), true);
                    return false;
                }
            }
        }
    }

    EditorOpenRequest request;
    request.resourcePath = path;
    request.resourceKey = path.empty()
        ? "workspace:game-flow:untitled" : path;
    request.displayPath = path.empty() ? "Untitled Game Flow" : path;
    request.assetType = "game-flow";
    request.preferredEditorId = kEditorGameFlowExtensionId;

    EditorDockViewOptions options;
    options.cardId = path.empty() ? "card_game_flow_untitled" : std::string{};
    const EditorDockOpenResult opened = _dockViewHost->open(request, options);
    if (!opened) {
        setAssetBrowserStatus(L"Game Flow Editor open failed: "
            + ayt::ui::decodeUtf8Text(opened.error), true);
        return false;
    }
    wirePromoteCallback();
    _ui.invalidateLayout();
    setAssetBrowserStatus(path.empty()
        ? L"Opened an untitled Game Flow"
        : L"Opened Game Flow: "
            + ayt::ui::decodeUtf8Text(
                std::filesystem::path(path).filename().string()));
    return true;
}

bool EditorSession::openUiFlowEditor(const std::string& requestedPath)
{
    if (_childWindows == nullptr || _workspace == nullptr) {
        setAssetBrowserStatus(
            L"UI Flow Editor requires the AYDevice child-window host", true);
        return false;
    }

    syncUiFlowDesignerLifetime();
    if (_uiFlowDesigner != nullptr && _uiFlowDesignerHandle != nullptr) {
        const bool sameDocument = requestedPath.empty()
            || EditorDocumentManager::normalizeResourceKey(requestedPath)
                == EditorDocumentManager::normalizeResourceKey(
                    _uiFlowDesignerDocument != nullptr
                        ? _uiFlowDesignerDocument->path() : std::string{});
        if (sameDocument) {
            (void)_workspace->documents().activate(_uiFlowDesignerDocumentId);
            (void)_childWindows->activateChildWindow(_uiFlowDesignerHandle);
            setAssetBrowserStatus(L"UI Flow Editor focused");
            return true;
        }
        if (!confirmUiFlowDesignerClose()) return false;
        const EditorChildWindowManager::Handle previousHandle =
            _uiFlowDesignerHandle;
        releaseUiFlowDesigner(false);
        (void)_childWindows->closeChildWindow(previousHandle);
    }

    std::string path = requestedPath;
    if (path.empty() && !_projectRoot.empty()) {
        std::string descriptorError;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(_projectRoot, &descriptorError);
        if (descriptor && !descriptor.ui.flow.empty()) {
            const std::filesystem::path configured =
                std::filesystem::path(_projectRoot)
                / (descriptor.assetRoot.empty() ? "Assets" : descriptor.assetRoot)
                / std::filesystem::path(descriptor.ui.flow);
            std::error_code existsError;
            if (std::filesystem::is_regular_file(configured, existsError)) {
                path = configured.string();
            }
        }
    }

    EditorOpenRequest request;
    request.resourcePath = path;
    request.resourceKey = path.empty()
        ? "workspace:ui-flow:untitled" : path;
    request.displayPath = path.empty() ? "Untitled UI Flow" : path;
    request.assetType = "ui-flow";
    request.preferredEditorId = kEditorUiFlowExtensionId;

    EditorOpenResult opened = _workspace->documents().open(request);
    if (!opened) {
        setAssetBrowserStatus(L"UI Flow Editor open failed: "
            + ayt::ui::decodeUtf8Text(opened.error), true);
        return false;
    }
    auto document = std::dynamic_pointer_cast<EditorUiFlowDocument>(
        opened.document);
    if (document == nullptr) {
        if (opened.status == EditorOpenStatus::Opened) {
            (void)_workspace->documents().close(
                opened.documentId, EditorDocumentCloseAction::Discard);
        }
        setAssetBrowserStatus(L"UI Flow Editor document type mismatch", true);
        return false;
    }

    _uiFlowDesignerDocumentId = opened.documentId;
    _uiFlowDesignerDocument = std::move(document);
    EditorUiFlowExtensionConfig controllerConfig;
    controllerConfig.assetRoot = resolveProjectAssetRoot(
        _assetDatabase.projectRoot());
    controllerConfig.openPathPicker = [this]() {
        HWND owner = _uiFlowDesignerHandle != nullptr
            ? static_cast<HWND>(_uiFlowDesignerHandle) : static_cast<HWND>(_hostWindow);
        return showUiFlowOpenDialog(owner,
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    controllerConfig.savePathPicker = [this]() {
        HWND owner = _uiFlowDesignerHandle != nullptr
            ? static_cast<HWND>(_uiFlowDesignerHandle) : static_cast<HWND>(_hostWindow);
        return showUiFlowSaveDialog(owner,
            (std::filesystem::path(_assetDatabase.projectRoot())
                / "Assets" / "ui").string());
    };
    controllerConfig.openLayoutForScreen = [this](
        const std::string& layoutAsset, std::string& message) {
        return openLayoutForFlowScreen(layoutAsset, message);
    };
    _uiFlowDesigner = std::make_unique<EditorUiFlowController>(
        _uiFlowDesignerDocument, std::move(controllerConfig));
    _uiFlowDesigner->setStateChanged([this]() {
        refreshUiFlowDesignerTitle();
    });

    ChildWindowConfig cfg;
    cfg.title = "AYUI Flow Editor";
    cfg.layoutPath = resolveUiFlowEditorChromePath(_engineAssetsRoot);
    cfg.x = 112;
    cfg.y = 80;
    cfg.width = 1440;
    cfg.height = 860;
    cfg.beforeCloseRequested = [this](ayt::ui::UIManager&) {
        return confirmUiFlowDesignerClose();
    };
    cfg.beforeClose = [this](ayt::ui::UIManager&) {
        releaseUiFlowDesigner(true);
    };

    EditorChildWindowManager::Handle handle = nullptr;
    if (!_childWindows->openChildWindow(cfg, handle) || handle == nullptr) {
        releaseUiFlowDesigner(true);
        setAssetBrowserStatus(L"UI Flow Editor window creation failed", true);
        return false;
    }
    _uiFlowDesignerHandle = handle;
    ayt::ui::UIManager* childUi = _childWindows->uiForHandle(handle);
    if (childUi == nullptr || !_uiFlowDesigner->attach(*childUi)) {
        (void)_childWindows->closeChildWindow(handle);
        setAssetBrowserStatus(L"UI Flow Editor UI attach failed", true);
        return false;
    }

    refreshUiFlowDesignerTitle();
    setAssetBrowserStatus(L"UI Flow Editor opened in a dedicated window");
    return true;
}

void EditorSession::syncUiFlowDesignerLifetime()
{
    if (_uiFlowDesignerHandle == nullptr || _childWindows == nullptr) return;
    for (const auto& entry : _childWindows->entries()) {
        if (entry.handle == _uiFlowDesignerHandle) return;
    }
    releaseUiFlowDesigner(true);
}

bool EditorSession::confirmUiFlowDesignerClose()
{
    if (_uiFlowDesignerDocument == nullptr || _workspace == nullptr) return true;
    EditorDocumentCloseAction action = EditorDocumentCloseAction::Discard;
    if (_uiFlowDesignerDocument->isDirty()) {
        if (static_cast<HWND>(_hostWindow) != nullptr) {
            const std::wstring prompt =
                ayt::ui::decodeUtf8Text(_uiFlowDesignerDocument->title())
                + L" has unsaved changes.\n\nSave before closing?";
            HWND owner = _uiFlowDesignerHandle != nullptr
                ? static_cast<HWND>(_uiFlowDesignerHandle) : static_cast<HWND>(_hostWindow);
            const int choice = ::MessageBoxW(
                owner, prompt.c_str(), L"AYUI Flow Editor",
                MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDCANCEL) return false;
            action = choice == IDYES
                ? EditorDocumentCloseAction::Save
                : EditorDocumentCloseAction::Discard;
            if (action == EditorDocumentCloseAction::Save
                && _uiFlowDesignerDocument->path().empty()) {
                const std::string destination = showUiFlowSaveDialog(owner,
                    (std::filesystem::path(_assetDatabase.projectRoot())
                        / "Assets" / "ui").string());
                if (destination.empty()) return false;
                std::string saveError;
                if (!_uiFlowDesignerDocument->saveAs(destination, &saveError)) {
                    setAssetBrowserStatus(L"UI Flow Editor save failed: "
                        + ayt::ui::decodeUtf8Text(saveError), true);
                    return false;
                }
                action = EditorDocumentCloseAction::Discard;
            }
        }
    }
    const EditorCloseResult closed = _workspace->documents().close(
        _uiFlowDesignerDocumentId, action);
    if (!closed) {
        if (closed.status == EditorCloseStatus::SaveFailed) {
            setAssetBrowserStatus(L"UI Flow Editor save failed: "
                + ayt::ui::decodeUtf8Text(closed.error), true);
        }
        return false;
    }
    return true;
}

void EditorSession::releaseUiFlowDesigner(bool closeDocument)
{
    const std::string documentId = _uiFlowDesignerDocumentId;
    if (_uiFlowDesigner != nullptr) _uiFlowDesigner->detach();
    _uiFlowDesigner.reset();
    _uiFlowDesignerDocument.reset();
    _uiFlowDesignerDocumentId.clear();
    _uiFlowDesignerHandle = nullptr;
    if (closeDocument && _workspace != nullptr && !documentId.empty()
        && _workspace->documents().find(documentId) != nullptr) {
        (void)_workspace->documents().close(
            documentId, EditorDocumentCloseAction::Discard);
    }
}

void EditorSession::refreshUiFlowDesignerTitle()
{
    if (_childWindows == nullptr || _uiFlowDesignerHandle == nullptr
        || _uiFlowDesignerDocument == nullptr) return;
    std::string title = "AYUI Flow Editor - "
        + _uiFlowDesignerDocument->title();
    if (_uiFlowDesignerDocument->isDirty()) title += " *";
    (void)_childWindows->setChildWindowTitle(_uiFlowDesignerHandle, title);
}

void EditorSession::syncAudioEditorLifetime() {
    if (_audioEditor == nullptr) {
        return;
    }
    if (_childWindows == nullptr || _audioEditorHandle == nullptr) {
    _audioEditor.reset();
    _audioEditorHandle = nullptr;
    releaseUiFlowDesigner(true);
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
    cfg.layoutPath = resolveAudioEditorChromePath(_engineAssetsRoot);
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
    HWND owner = static_cast<HWND>(_hostWindow);
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
    if (static_cast<HWND>(_hostWindow) != nullptr) {
        ::PostMessageW(static_cast<HWND>(_hostWindow), WM_CLOSE, 0, 0);
    }
}

void EditorSession::requestHostMinimize() {
    if (static_cast<HWND>(_hostWindow) != nullptr) {
        ::ShowWindow(static_cast<HWND>(_hostWindow), SW_MINIMIZE);
    }
}

void EditorSession::requestHostMaximizeToggle() {
    if (static_cast<HWND>(_hostWindow) == nullptr) {
        return;
    }
    if (::IsZoomed(static_cast<HWND>(_hostWindow))) {
        ::ShowWindow(static_cast<HWND>(_hostWindow), SW_RESTORE);
    } else {
        ::ShowWindow(static_cast<HWND>(_hostWindow), SW_MAXIMIZE);
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
        ImportDialog::showOpenFileDialog(static_cast<HWND>(_hostWindow));
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

void EditorSession::refreshInspectorLabels()
{
    refreshComponentBrowser();
    if (_selectedAssetId != 0) {
        setInspectorAssetMode(true);
        refreshAssetInspector();
        return;
    }
    setInspectorAssetMode(false);

    if (!_selection.empty()) {
        if (auto* w = hierarchyWorldMutable()) {
            if (ayt::entity::Entity* sel = _selection.resolve(w)) {
                const char* nm = sel->getName();
                setInspectorHint(ayt::ui::decodeUtf8Text(
                    std::string(_gameView.mode() == EditorMode::Edit
                                    ? "Entity: " : "Play entity: ")
                    + ((nm && nm[0]) ? nm : "Unnamed")));
                return;
            }
        }
        clearSelectedEntity(false);
    }

    if (_gameView.mode() != EditorMode::Edit) {
        setInspectorHint(L"Locked during Play.");
        return;
    }
    setInspectorHint(L"No entity selected");
}

void EditorSession::bindComponentBrowser()
{
    _componentPicker = dynamic_cast<ayt::ui::ComboBox*>(
        _ui.findById("cmb_add_component"));
    _attachedComponentPicker = dynamic_cast<ayt::ui::ComboBox*>(
        _ui.findById("cmb_attached_component"));
    _componentPropertyBody = dynamic_cast<ayt::ui::VBox*>(
        _ui.findById("inspector_component_properties"));
    if (_attachedComponentPicker != nullptr) {
        _attachedComponentPicker->setOnSelectionChanged([this](int index) {
            if (_updatingComponentPicker || index < 0
                || static_cast<std::size_t>(index)
                    >= _attachedComponentTypeNames.size()) {
                return;
            }
            _inspectedComponentTypeName = _attachedComponentTypeNames[
                static_cast<std::size_t>(index)];
            // Selection changes also change dependency-aware remove state.
            // Rebuild the browser under the existing re-entry guard so the
            // button cannot retain the previous component's policy.
            refreshComponentBrowser();
        });
    }
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_add_component"))) {
        button->setOnClicked([this]() { addSelectedComponent(); });
    }
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_remove_component"))) {
        button->setOnClicked([this]() { removeSelectedComponent(); });
    }
    refreshComponentBrowser();
}

void EditorSession::refreshComponentBrowser()
{
    ayt::entity::Entity* entity = nullptr;
    if (_selectedAssetId == 0) {
        entity = _selection.resolve(hierarchyWorldMutable());
    }
    const bool canAdd = entity != nullptr
        && _gameView.mode() == EditorMode::Edit;

    std::vector<const ayt::entity::ComponentDescriptor*> attached;
    std::vector<const ayt::entity::ComponentDescriptor*> available;
    for (const auto& descriptor :
         ayt::entity::ComponentRegistry::instance().descriptors()) {
        const bool present = entity != nullptr && descriptor.has != nullptr
            && descriptor.has(*entity);
        if (present) attached.push_back(&descriptor);
        if (canAdd && descriptor.editorAddable && descriptor.add != nullptr
            && !present) {
            available.push_back(&descriptor);
        }
    }
    auto descriptorLess = [](const auto* left, const auto* right) {
        if (left->category != right->category) {
            return left->category < right->category;
        }
        if (left->displayName != right->displayName) {
            return left->displayName < right->displayName;
        }
        return left->name < right->name;
    };
    std::sort(attached.begin(), attached.end(), descriptorLess);
    std::sort(available.begin(), available.end(), descriptorLess);

    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
            _ui.findById("inspector_components"))) {
        const std::string text = entity == nullptr
            ? "Components"
            : "Components (" + std::to_string(attached.size()) + ")";
        label->setText(ayt::ui::decodeUtf8Text(text));
    }

    _attachedComponentTypeNames.clear();
    std::vector<std::wstring> attachedItems;
    attachedItems.reserve(attached.size());
    _attachedComponentTypeNames.reserve(attached.size());
    int inspectedIndex = -1;
    for (std::size_t i = 0; i < attached.size(); ++i) {
        const auto* descriptor = attached[i];
        attachedItems.push_back(ayt::ui::decodeUtf8Text(
            descriptor->category + " / " + descriptor->displayName));
        _attachedComponentTypeNames.push_back(descriptor->name);
        if (descriptor->name == _inspectedComponentTypeName) {
            inspectedIndex = static_cast<int>(i);
        }
    }
    if (attachedItems.empty()) {
        attachedItems.push_back(entity == nullptr
            ? L"Select an entity" : L"No components attached");
        _inspectedComponentTypeName.clear();
    } else if (inspectedIndex < 0) {
        inspectedIndex = 0;
        for (std::size_t i = 0; i < attached.size(); ++i) {
            if (attached[i]->name == "Transform") {
                inspectedIndex = static_cast<int>(i);
                break;
            }
        }
        _inspectedComponentTypeName = _attachedComponentTypeNames[
            static_cast<std::size_t>(inspectedIndex)];
    }
    _updatingComponentPicker = true;
    if (_attachedComponentPicker != nullptr) {
        _attachedComponentPicker->setItems(attachedItems);
        _attachedComponentPicker->setSelectedIndex(
            attached.empty() ? 0 : inspectedIndex);
        _attachedComponentPicker->setEnabled(!attached.empty());
    }
    _updatingComponentPicker = false;

    const auto* inspectedDescriptor = _inspectedComponentTypeName.empty()
        ? nullptr
        : ayt::entity::ComponentRegistry::instance().find(
            _inspectedComponentTypeName);
    std::string removeReason;
    const bool mechanicallyRemovable = canAdd && inspectedDescriptor != nullptr
        && inspectedDescriptor->remove != nullptr
        && inspectedDescriptor->has != nullptr
        && inspectedDescriptor->has(*entity);
    const bool canRemove = mechanicallyRemovable
        && EditorComponentPolicyRegistry::instance().canRemove(
            *entity, _inspectedComponentTypeName, &removeReason);
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_remove_component"))) {
        button->setEnabled(canRemove);
        button->setAccessibilityDescription(canRemove
            ? L"Remove the selected component"
            : (removeReason.empty()
                   ? L"The selected component cannot be removed"
                   : ayt::ui::decodeUtf8Text(removeReason)));
    }

    _componentPickerTypeNames.clear();
    std::vector<std::wstring> items;
    items.reserve(available.size());
    _componentPickerTypeNames.reserve(available.size());
    for (const auto* descriptor : available) {
        items.push_back(ayt::ui::decodeUtf8Text(
            descriptor->category + " / " + descriptor->displayName));
        _componentPickerTypeNames.push_back(descriptor->name);
    }
    if (items.empty()) {
        items.push_back(entity == nullptr
            ? L"Select an entity" : L"All components added");
    }
    if (_componentPicker != nullptr) {
        _componentPicker->setItems(items);
        _componentPicker->setSelectedIndex(0);
        _componentPicker->setEnabled(canAdd && !available.empty());
    }
    if (auto* button = dynamic_cast<ayt::ui::Button*>(
            _ui.findById("btn_add_component"))) {
        button->setEnabled(canAdd && !available.empty());
    }
    rebuildComponentPropertyEditor();
}

void EditorSession::addSelectedComponent()
{
    if (_componentPicker == nullptr || _gameView.mode() != EditorMode::Edit
        || _document == nullptr) {
        return;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    const int selected = _componentPicker->getSelectedIndex();
    if (world == nullptr || entity == nullptr || selected < 0
        || static_cast<std::size_t>(selected)
            >= _componentPickerTypeNames.size()) {
        return;
    }

    const std::string typeName = _componentPickerTypeNames[
        static_cast<std::size_t>(selected)];
    std::vector<std::string> added;
    std::string error;
    if (!_document->addComponent(
            entity->getId(), typeName, &added, &error)) {
        if (!error.empty()) setInspectorHint(ayt::ui::decodeUtf8Text(error));
        refreshComponentBrowser();
        return;
    }

    _inspectedComponentTypeName = typeName;
    refreshInspectorLabels();
    refreshUnsavedIndicator();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::removeSelectedComponent()
{
    if (_gameView.mode() != EditorMode::Edit || _document == nullptr
        || _inspectedComponentTypeName.empty()) {
        return;
    }
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    const auto* descriptor = ayt::entity::ComponentRegistry::instance().find(
        _inspectedComponentTypeName);
    if (world == nullptr || entity == nullptr || descriptor == nullptr
        || descriptor->has == nullptr || descriptor->remove == nullptr
        || !descriptor->has(*entity)) {
        refreshComponentBrowser();
        return;
    }
    std::string removeReason;
    if (!EditorComponentPolicyRegistry::instance().canRemove(
            *entity, _inspectedComponentTypeName, &removeReason)) {
        setInspectorHint(ayt::ui::decodeUtf8Text(removeReason));
        refreshComponentBrowser();
        return;
    }
    if (_inspectedComponentTypeName == "Transform") {
        finishTransformGizmoDrag(false);
    }
    std::string error;
    if (!_document->removeComponent(
            entity->getId(), _inspectedComponentTypeName, &error)) {
        if (!error.empty()) setInspectorHint(ayt::ui::decodeUtf8Text(error));
        refreshComponentBrowser();
        return;
    }
    _inspectedComponentTypeName.clear();
    refreshInspectorLabels();
    refreshUnsavedIndicator();
    syncTransformGizmoToRenderer();
    if (_repaintCallback) _repaintCallback();
}

void EditorSession::rebuildComponentPropertyEditor()
{
    if (_componentPropertyBody == nullptr) return;

    const EditorDensityMetrics& density =
        editorDensityMetrics(_preferences.density);

    if (ayt::ui::Widget* focused = _ui.getFocusedWidget();
        focused != nullptr && isDescendantOf(focused, _componentPropertyBody)) {
        _ui.clearFocusNoDispatch(focused);
    }
    const std::vector<ayt::ui::Widget*> oldChildren =
        _componentPropertyBody->getChildren();
    for (ayt::ui::Widget* child : oldChildren) {
        _componentPropertyBody->removeWidget(child);
        ayt::ui::destroyWidgetTree(child);
    }

    auto addLabel = [this, &density](const std::wstring& text, float height,
                                    const std::string& id = {}) {
        auto* label = new ayt::ui::TextLabel();
        if (!id.empty()) label->setId(id);
        label->setText(text);
        label->setFontSize(density.bodyFontSize);
        label->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        label->setSize({240.0f, height});
        _componentPropertyBody->addWidget(label, height);
        return label;
    };

    ayt::entity::Entity* entity = _selection.resolve(hierarchyWorldMutable());
    const auto* descriptor = _inspectedComponentTypeName.empty()
        ? nullptr
        : ayt::entity::ComponentRegistry::instance().find(
            _inspectedComponentTypeName);
    ayt::entity::IComponent* component = entity != nullptr
        && descriptor != nullptr && descriptor->get != nullptr
        ? descriptor->get(*entity) : nullptr;
    if (component == nullptr) {
        addLabel(entity == nullptr ? L"Select an entity"
                                   : L"Entity has no components. It remains in Hierarchy.",
                 20.0f, "inspector_property_placeholder");
        _ui.invalidateLayout();
        return;
    }

    addLabel(ayt::ui::decodeUtf8Text(descriptor->displayName), 22.0f,
             "inspector_property_header");
    auto* type = ayt::reflect::TypeRegistryImpl::instance().findType(
        descriptor->type.hash_code());
    if (type == nullptr || type->getFieldCount() == 0) {
        addLabel(L"No reflected properties", 20.0f,
                 "inspector_property_placeholder");
        _ui.invalidateLayout();
        return;
    }

    const bool sessionReadOnly = _gameView.mode() != EditorMode::Edit;
    std::size_t visibleFieldCount = 0;
    const std::string componentTypeName = descriptor->name;
    for (std::uint32_t fieldIndex = 0;
         fieldIndex < type->getFieldCount(); ++fieldIndex) {
        ayt::reflect::IFieldInfo* field = type->getField(fieldIndex);
        if (field == nullptr
            || field->hasAttribute(ayt::reflect::FieldAttribute::Hidden)) {
            continue;
        }
        void* fieldValue = field->get(component);
        ayt::reflect::ITypeInfo* fieldType = field->getType();
        if (fieldValue == nullptr || fieldType == nullptr) continue;
        ++visibleFieldCount;

        const std::string fieldName = field->getName();
        const char* reflectedDisplayName = field->getDisplayName();
        std::wstring displayName = ayt::ui::decodeUtf8Text(
            reflectedDisplayName != nullptr && reflectedDisplayName[0] != '\0'
                ? reflectedDisplayName : field->getName());
        const char* reflectedTooltip = field->getTooltip();
        const std::wstring tooltip = ayt::ui::decodeUtf8Text(
            reflectedTooltip != nullptr ? reflectedTooltip : "");
        if (field->hasAttribute(ayt::reflect::FieldAttribute::Angle)) {
            displayName += L" (degrees)";
        }
        const bool readOnly = sessionReadOnly
            || field->hasAttribute(
                ayt::reflect::FieldAttribute::BlueprintReadOnly);

        int elementCount = 0;
        bool angleValues = false;
        if (isReflectedType<ayt::math::FVector2>(fieldType)) {
            elementCount = 2;
        } else if (isReflectedType<ayt::math::FVector3>(fieldType)) {
            elementCount = 3;
        } else if (isReflectedType<ayt::math::FVector4>(fieldType)) {
            elementCount = 4;
        } else if (isReflectedType<ayt::math::FQuaternion>(fieldType)) {
            elementCount = 3;
            angleValues = true;
        }

        auto makeInput = [this, componentTypeName, fieldName, readOnly](
                             const std::wstring& valueText,
                             int elementIndex,
                             bool numeric = false,
                             bool integral = false,
                             bool unsignedIntegral = false,
                             int significantDigits = 9) {
            ayt::ui::TextInput* input = numeric
                ? static_cast<ayt::ui::TextInput*>(new InspectorNumberInput())
                : new ayt::ui::TextInput();
            input->setStyleId("editor_property_input");
            if (auto* number = dynamic_cast<InspectorNumberInput*>(input)) {
                number->setInspectorText(valueText, integral,
                                         unsignedIntegral,
                                         significantDigits);
                number->setOnNumericScrubDelta([number](double delta) {
                    number->applyInspectorScrubDelta(delta);
                });
            } else {
                input->setText(valueText);
            }
            input->setReadOnly(readOnly);
            input->setNumericScrubEnabled(numeric);
            input->setSize({70.0f, 26.0f});
            input->setOnSubmit(
                [this, componentTypeName, fieldName, elementIndex](
                    const std::wstring& value) {
                    commitInspectorTextField(
                        componentTypeName, fieldName, elementIndex, value);
                });
            input->setOnFocusLostNotify(
                [this, componentTypeName, fieldName, elementIndex, input]() {
                    commitInspectorTextField(componentTypeName, fieldName,
                                             elementIndex, input->getText());
                });
            return input;
        };

        if (elementCount > 0) {
            auto* row = new ayt::ui::HBox();
            row->setId("inspector_field_" + fieldName);
            row->setSpacing(1.0f);
            row->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
            row->setSize({240.0f, 26.0f});
            float values[4]{};
            if (isReflectedType<ayt::math::FVector2>(fieldType)) {
                const auto& vector =
                    *static_cast<ayt::math::FVector2*>(fieldValue);
                values[0] = vector.x;
                values[1] = vector.y;
            } else if (isReflectedType<ayt::math::FVector3>(fieldType)) {
                const auto& vector =
                    *static_cast<ayt::math::FVector3*>(fieldValue);
                values[0] = vector.x;
                values[1] = vector.y;
                values[2] = vector.z;
            } else if (isReflectedType<ayt::math::FVector4>(fieldType)) {
                const auto& vector =
                    *static_cast<ayt::math::FVector4*>(fieldValue);
                values[0] = vector.x;
                values[1] = vector.y;
                values[2] = vector.z;
                values[3] = vector.w;
            } else {
                constexpr float radiansToDegrees = 57.29577951308232f;
                const auto euler = static_cast<ayt::math::FQuaternion*>(
                    fieldValue)->toEulerAngles();
                values[0] = euler.x * radiansToDegrees;
                values[1] = euler.y * radiansToDegrees;
                values[2] = euler.z * radiansToDegrees;
            }

            // The Inspector's 240-DIP baseline is too narrow for a label
            // plus four useful editors. Vector4 keeps its label above as the
            // compact-width fallback; Vector2/3 and Euler values use one row.
            if (elementCount == 4) {
                auto* label = addLabel(displayName, 18.0f,
                    "inspector_field_label_" + fieldName);
                if (!tooltip.empty()) label->setAccessibilityDescription(tooltip);
            } else {
                auto* fieldLabel = new ayt::ui::TextLabel();
                fieldLabel->setId("inspector_field_label_" + fieldName);
                fieldLabel->setText(displayName);
                fieldLabel->setFontSize(density.bodyFontSize);
                fieldLabel->setTextColor(
                    ayt::math::FVector4(0.82f, 0.85f, 0.90f, 1.0f));
                fieldLabel->setVerticalAlignment(
                    ayt::ui::TextLabel::VAlignment::Center);
                if (!tooltip.empty()) {
                    fieldLabel->setAccessibilityDescription(tooltip);
                }
                row->addWidget(fieldLabel, 46.0f);
            }

            static constexpr wchar_t axisNames[] = {L'X', L'Y', L'Z', L'W'};
            static constexpr float axisColors[][3] = {
                {0.86f, 0.42f, 0.42f},
                {0.48f, 0.75f, 0.43f},
                {0.42f, 0.60f, 0.90f},
                {0.70f, 0.72f, 0.78f},
            };
            std::vector<InspectorNumberInput*> vectorInputs;
            for (int element = 0; element < elementCount; ++element) {
                auto* axis = new ayt::ui::TextLabel();
                axis->setId("inspector_field_axis_" + fieldName + "_"
                            + std::to_string(element));
                std::wstring axisText(1, axisNames[element]);
                if (angleValues) axisText += L"°";
                axis->setText(axisText);
                axis->setFontSize(density.secondaryFontSize);
                axis->setTextColor(ayt::math::FVector4(
                    axisColors[element][0], axisColors[element][1],
                    axisColors[element][2], 1.0f));
                axis->setHorizontalAlignment(
                    ayt::ui::TextLabel::HAlignment::Center);
                axis->setVerticalAlignment(
                    ayt::ui::TextLabel::VAlignment::Center);
                row->addWidget(axis, angleValues ? 12.0f : 9.0f);

                auto* input = makeInput(formatPreciseFloat(values[element]), element,
                                        true);
                input->setId("inspector_field_" + fieldName + "_"
                             + std::to_string(element));
                row->addWidget(input);
                vectorInputs.push_back(
                    static_cast<InspectorNumberInput*>(input));
                if (!tooltip.empty()) input->setAccessibilityDescription(tooltip);
            }
            for (InspectorNumberInput* input : vectorInputs) {
                input->setFocusLayoutCallback(
                    [this, row, input, vectorInputs](bool focused) {
                        for (InspectorNumberInput* peer : vectorInputs) {
                            const int slot = row->slotIndexOf(peer);
                            if (slot < 0) continue;
                            row->setSlotSize(slot,
                                focused && peer != input ? 32.0f : 0.0f);
                        }
                        _ui.invalidateLayout();
                        if (_repaintCallback) _repaintCallback();
                    });
            }
            _componentPropertyBody->addWidget(row, 26.0f);

            if (elementCount == 4
                && field->hasAttribute(ayt::reflect::FieldAttribute::Color)) {
                auto* picker = new ayt::ui::ColorPicker();
                picker->setId("inspector_color_" + fieldName);
                picker->setSize({240.0f, 224.0f});
                picker->setColor(*static_cast<ayt::math::FVector4*>(fieldValue),
                                 false);
                if (!tooltip.empty()) picker->setAccessibilityDescription(tooltip);
                if (!readOnly) {
                    picker->setOnColorCommitted(
                        [this, componentTypeName, fieldName](
                            const ayt::math::FVector4& color) {
                            commitInspectorColorField(
                                componentTypeName, fieldName, color);
                        });
                }
                _componentPropertyBody->addWidget(picker, 224.0f);
            }
            continue;
        }

        auto* scalarLabel = addLabel(displayName, 18.0f,
            "inspector_field_label_" + fieldName);
        if (!tooltip.empty()) scalarLabel->setAccessibilityDescription(tooltip);

        if (isReflectedType<bool>(fieldType)) {
            auto* check = new ayt::ui::CheckBox();
            check->setId("inspector_field_" + fieldName);
            check->setText(L"Value");
            check->setChecked(*static_cast<bool*>(fieldValue));
            check->setEnabled(!readOnly);
            check->setOnToggled(
                [this, componentTypeName, fieldName](bool checked) {
                    commitInspectorBoolField(
                        componentTypeName, fieldName, checked);
                });
            check->setSize({240.0f, 24.0f});
            _componentPropertyBody->addWidget(check, 24.0f);
            continue;
        }

        std::wstring valueText;
        bool editableText = true;
        bool numericScalar = false;
        bool integralScalar = false;
        bool unsignedIntegralScalar = false;
        bool scrubbableScalar = false;
        int scalarSignificantDigits = 9;
        double scalarValue = 0.0;
        if (isReflectedType<std::string>(fieldType)) {
            valueText = ayt::ui::decodeUtf8Text(
                *static_cast<std::string*>(fieldValue));
        } else if (isReflectedType<float>(fieldType)) {
            scalarValue = *static_cast<float*>(fieldValue);
            numericScalar = true;
            scrubbableScalar = true;
            valueText = formatPreciseFloat(static_cast<float>(scalarValue));
        } else if (isReflectedType<double>(fieldType)) {
            scalarValue = *static_cast<double*>(fieldValue);
            numericScalar = true;
            scrubbableScalar = true;
            scalarSignificantDigits = 17;
            valueText = formatPreciseDouble(scalarValue);
        } else if (isReflectedType<std::int8_t>(fieldType)) {
            scalarValue = *static_cast<std::int8_t*>(fieldValue);
            numericScalar = true;
            integralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(static_cast<int>(scalarValue));
        } else if (isReflectedType<std::uint8_t>(fieldType)) {
            scalarValue = *static_cast<std::uint8_t*>(fieldValue);
            numericScalar = true;
            integralScalar = true;
            unsignedIntegralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(static_cast<unsigned>(scalarValue));
        } else if (isReflectedType<std::int16_t>(fieldType)) {
            scalarValue = *static_cast<std::int16_t*>(fieldValue);
            numericScalar = true;
            integralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(static_cast<int>(scalarValue));
        } else if (isReflectedType<std::uint16_t>(fieldType)) {
            scalarValue = *static_cast<std::uint16_t*>(fieldValue);
            numericScalar = true;
            integralScalar = true;
            unsignedIntegralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(static_cast<unsigned>(scalarValue));
        } else if (isReflectedType<std::int32_t>(fieldType)) {
            scalarValue = *static_cast<std::int32_t*>(fieldValue);
            numericScalar = true;
            integralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(static_cast<std::int64_t>(scalarValue));
        } else if (isReflectedType<std::uint32_t>(fieldType)) {
            scalarValue = *static_cast<std::uint32_t*>(fieldValue);
            numericScalar = true;
            integralScalar = true;
            unsignedIntegralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(static_cast<std::uint64_t>(scalarValue));
        } else if (isReflectedType<std::int64_t>(fieldType)) {
            scalarValue = static_cast<double>(
                *static_cast<std::int64_t*>(fieldValue));
            numericScalar = true;
            integralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(
                *static_cast<std::int64_t*>(fieldValue));
        } else if (isReflectedType<std::uint64_t>(fieldType)) {
            scalarValue = static_cast<double>(
                *static_cast<std::uint64_t*>(fieldValue));
            numericScalar = true;
            integralScalar = true;
            unsignedIntegralScalar = true;
            scrubbableScalar = true;
            valueText = std::to_wstring(
                *static_cast<std::uint64_t*>(fieldValue));
        } else if (auto* container = dynamic_cast<
                       ayt::reflect::IContainerTypeInfo*>(fieldType)) {
            valueText = std::to_wstring(
                container->getContainerSize(fieldValue))
                + L" items (read only)";
            editableText = false;
        } else {
            valueText = ayt::ui::decodeUtf8Text(fieldType->getName())
                + L" (read only)";
            editableText = false;
        }

        const std::vector<std::wstring> enumItems =
            editor2DEnumItems(componentTypeName, fieldName);
        if (numericScalar && !enumItems.empty()) {
            auto* combo = new ayt::ui::ComboBox();
            combo->setId("inspector_field_" + fieldName);
            combo->setItems(enumItems);
            const int selected = static_cast<int>(std::llround(scalarValue));
            combo->setSelectedIndex(selected >= 0
                    && selected < static_cast<int>(enumItems.size())
                ? selected : -1);
            combo->setEnabled(!readOnly);
            combo->setSize({240.0f, 26.0f});
            if (!tooltip.empty()) combo->setAccessibilityDescription(tooltip);
            combo->setOnSelectionChanged(
                [this, componentTypeName, fieldName](int index) {
                    if (index < 0) return;
                    commitInspectorTextField(componentTypeName, fieldName, -1,
                                             std::to_wstring(index));
                });
            _componentPropertyBody->addWidget(combo, 26.0f);
            continue;
        }

        if (numericScalar
            && field->hasAttribute(ayt::reflect::FieldAttribute::Slider)
            && std::isfinite(field->getMinValue())
            && std::isfinite(field->getMaxValue())
            && field->getMaxValue() > field->getMinValue()) {
            auto* row = new ayt::ui::HBox();
            row->setId("inspector_field_" + fieldName);
            row->setSpacing(5.0f);
            row->setSize({240.0f, 26.0f});
            auto* slider = new ayt::ui::Slider();
            slider->setId("inspector_field_slider_" + fieldName);
            slider->setValueRange(field->getMinValue(), field->getMaxValue());
            slider->setValue(static_cast<float>(scalarValue));
            slider->setEnabled(!readOnly);
            if (!tooltip.empty()) slider->setAccessibilityDescription(tooltip);
            row->addWidget(slider);
            auto* input = makeInput(valueText, -1, true, integralScalar,
                                    unsignedIntegralScalar,
                                    scalarSignificantDigits);
            input->setSize({68.0f, 26.0f});
            input->setId("inspector_field_value_" + fieldName);
            const float step = field->getStep();
            slider->setOnValueChanged(
                [this, componentTypeName, fieldName, input, step](float value) {
                    if (step > 0.0f) value = std::round(value / step) * step;
                    std::wstring text = formatPreciseFloat(value);
                    if (auto* number = dynamic_cast<InspectorNumberInput*>(input)) {
                        number->setInspectorValue(static_cast<double>(value));
                        text = number->preciseText();
                    } else {
                        input->setText(text);
                    }
                    commitInspectorTextField(
                        componentTypeName, fieldName, -1, text);
                });
            row->addWidget(input, 68.0f);
            _componentPropertyBody->addWidget(row, 26.0f);
            continue;
        }

        if (isReflectedType<std::string>(fieldType)
            && (field->hasAttribute(ayt::reflect::FieldAttribute::Asset)
                || isEditor2DResourceField(componentTypeName, fieldName))) {
            auto* row = new ayt::ui::HBox();
            row->setId("inspector_field_" + fieldName);
            row->setSpacing(4.0f);
            row->setSize({240.0f, 26.0f});
            auto* input = makeInput(valueText, -1, false);
            input->setId("inspector_field_value_" + fieldName);
            if (!tooltip.empty()) input->setAccessibilityDescription(tooltip);
            row->addWidget(input);
            auto* browse = new ayt::ui::Button();
            browse->setStyleId("editor_parameter_button");
            browse->setText(L"...");
            browse->setSize({30.0f, 26.0f});
            browse->setEnabled(!readOnly);
            browse->setAccessibilityLabel(L"Choose project asset");
            browse->setOnClicked(
                [this, componentTypeName, fieldName, input]() {
                    const std::string selected = showAssetReferenceDialog(
                        static_cast<HWND>(_hostWindow), _assetDatabase.projectRoot());
                    if (selected.empty()) return;
                    std::string reference =
                        _assetDatabase.portableAssetPath(selected);
                    if (componentTypeName == "TilemapComponent"
                        && fieldName == "tilemapPath"
                        && endsWithInsensitive(selected, ".aytilemap.json")) {
                        std::filesystem::path stem(selected);
                        stem = stem.stem().stem();
                        reference = ayt::resource::makeTilemapVirtualPath(
                            stem.string());
                    }
                    const std::wstring portable =
                        ayt::ui::decodeUtf8Text(reference);
                    input->setText(portable);
                    commitInspectorTextField(
                        componentTypeName, fieldName, -1, portable);
                });
            row->addWidget(browse, 30.0f);
            _componentPropertyBody->addWidget(row, 26.0f);
            continue;
        }
        auto* input = makeInput(valueText, -1, numericScalar, integralScalar,
                                unsignedIntegralScalar,
                                scalarSignificantDigits);
        input->setId("inspector_field_" + fieldName);
        input->setReadOnly(readOnly || !editableText);
        input->setNumericScrubEnabled(editableText && scrubbableScalar);
        input->setSize({240.0f, 26.0f});
        if (!tooltip.empty()) input->setAccessibilityDescription(tooltip);
        _componentPropertyBody->addWidget(input, 26.0f);
    }
    if (visibleFieldCount == 0) {
        addLabel(L"No visible reflected properties", 20.0f,
                 "inspector_property_placeholder");
    }
    _ui.invalidateLayout();
}

void EditorSession::commitInspectorColorField(
    const std::string& componentType, const std::string& fieldName,
    const ayt::math::FVector4& next)
{
    if (_gameView.mode() != EditorMode::Edit || _document == nullptr) return;
    ayt::entity::Entity* entity = _selection.resolve(hierarchyWorldMutable());
    const auto* descriptor = ayt::entity::ComponentRegistry::instance().find(
        componentType);
    ayt::entity::IComponent* component = entity != nullptr
        && descriptor != nullptr && descriptor->get != nullptr
        ? descriptor->get(*entity) : nullptr;
    auto* type = descriptor != nullptr
        ? ayt::reflect::TypeRegistryImpl::instance().findType(
            descriptor->type.hash_code()) : nullptr;
    auto* field = type != nullptr ? type->findField(fieldName.c_str()) : nullptr;
    if (component == nullptr || field == nullptr
        || field->hasAttribute(ayt::reflect::FieldAttribute::Hidden)
        || field->hasAttribute(ayt::reflect::FieldAttribute::BlueprintReadOnly)
        || !isReflectedType<ayt::math::FVector4>(field->getType())) {
        return;
    }
    auto* current = static_cast<ayt::math::FVector4*>(field->get(component));
    if (current == nullptr || *current == next) return;
    (void)_document->mutateComponent(
        entity->getId(), componentType, "Edit " + fieldName,
        "property:" + std::to_string(entity->getId()) + ":"
            + componentType + ":" + fieldName,
        [field, next](ayt::entity::IComponent& editable) {
            auto* target = static_cast<ayt::math::FVector4*>(
                field->get(&editable));
            if (target == nullptr || *target == next) return false;
            *target = next;
            return true;
        });
}

void EditorSession::commitInspectorTextField(
    const std::string& componentType,
    const std::string& fieldName,
    int elementIndex,
    const std::wstring& text)
{
    if (_gameView.mode() != EditorMode::Edit || _document == nullptr) return;
    ayt::entity::World* world = hierarchyWorldMutable();
    ayt::entity::Entity* entity = _selection.resolve(world);
    const auto* descriptor = ayt::entity::ComponentRegistry::instance().find(
        componentType);
    ayt::entity::IComponent* component = entity != nullptr
        && descriptor != nullptr && descriptor->get != nullptr
        ? descriptor->get(*entity) : nullptr;
    auto* type = descriptor != nullptr
        ? ayt::reflect::TypeRegistryImpl::instance().findType(
            descriptor->type.hash_code()) : nullptr;
    auto* field = type != nullptr ? type->findField(fieldName.c_str()) : nullptr;
    if (world == nullptr || entity == nullptr || component == nullptr
        || field == nullptr || field->getType() == nullptr
        || field->hasAttribute(ayt::reflect::FieldAttribute::Hidden)
        || field->hasAttribute(
            ayt::reflect::FieldAttribute::BlueprintReadOnly)) {
        return;
    }
    void* value = field->get(component);
    auto* fieldType = field->getType();
    if (value == nullptr) return;

    const bool stringField = isReflectedType<std::string>(fieldType);
    double parsed = 0.0;
    if (!stringField && !parseDouble(text, parsed)) return;
    bool constrainedIntegral = false;
    if ((componentType == "SpriteComponent"
         || componentType == "TilemapComponent")
        && fieldName == "layer") {
        parsed = std::clamp(parsed, 0.0, 31.0);
        constrainedIntegral = true;
    } else if ((componentType == "SpriteComponent"
                || componentType == "TilemapComponent")
               && fieldName == "sortingKey") {
        parsed = std::clamp(parsed, 0.0, 16777215.0);
        constrainedIntegral = true;
    } else if (componentType == "OrthoCameraComponent"
               && (fieldName == "zoom" || fieldName == "viewSize")) {
        parsed = std::max(parsed, 0.0001);
    }

    if (componentType == "Transform") {
        auto* transform = entity->getComponent<ayt::entity::Transform>();
        if (transform != nullptr) {
            EditorTransformState state{
                transform->position, transform->rotation, transform->scale};
            bool handled = false;
            if (fieldName == "position" && elementIndex >= 0
                && elementIndex < 3) {
                state.position[elementIndex] = static_cast<float>(parsed);
                handled = true;
            } else if (fieldName == "scale" && elementIndex >= 0
                       && elementIndex < 3) {
                state.scale[elementIndex] = static_cast<float>(parsed);
                handled = true;
            } else if (fieldName == "rotation" && elementIndex >= 0
                       && elementIndex < 3) {
                constexpr float degreesToRadians = 0.017453292519943295f;
                auto euler = transform->rotation.toEulerAngles();
                euler[elementIndex] = static_cast<float>(parsed)
                    * degreesToRadians;
                state.rotation = ayt::math::FQuaternion::fromEulerAngles(euler);
                handled = true;
            }
            if (handled) {
                _updatingComponentPropertyCommit = true;
                (void)_document->executeTransform(
                    entity->getId(), state, "Edit Transform",
                    "transform:" + std::to_string(entity->getId())
                        + ":" + fieldName + ":"
                        + std::to_string(elementIndex));
                _updatingComponentPropertyCommit = false;
                return;
            }
        }
    }

    const bool changed = _document->mutateComponent(
        entity->getId(), componentType, "Edit " + fieldName,
        "property:" + std::to_string(entity->getId()) + ":"
            + componentType + ":" + fieldName + ":"
            + std::to_string(elementIndex),
        [&](ayt::entity::IComponent& editable) {
            void* targetValue = field->get(&editable);
            if (targetValue == nullptr) return false;
            bool assigned = false;
            if (stringField) {
                auto& target = *static_cast<std::string*>(targetValue);
                const std::string next = wideToUtf8(text);
                assigned = target != next;
                if (assigned) target = next;
            } else if (isReflectedType<float>(fieldType)) {
                auto& target = *static_cast<float*>(targetValue);
                const float next = static_cast<float>(parsed);
                assigned = target != next;
                if (assigned) target = next;
            } else if (isReflectedType<double>(fieldType)) {
                auto& target = *static_cast<double*>(targetValue);
                assigned = target != parsed;
                if (assigned) target = parsed;
            } else if (isReflectedType<ayt::math::FVector2>(fieldType)
                       && elementIndex >= 0 && elementIndex < 2) {
                auto& target =
                    *static_cast<ayt::math::FVector2*>(targetValue);
                const float next = static_cast<float>(parsed);
                assigned = target[elementIndex] != next;
                if (assigned) target[elementIndex] = next;
            } else if (isReflectedType<ayt::math::FVector3>(fieldType)
                       && elementIndex >= 0 && elementIndex < 3) {
                auto& target =
                    *static_cast<ayt::math::FVector3*>(targetValue);
                const float next = static_cast<float>(parsed);
                assigned = target[elementIndex] != next;
                if (assigned) target[elementIndex] = next;
            } else if (isReflectedType<ayt::math::FVector4>(fieldType)
                       && elementIndex >= 0 && elementIndex < 4) {
                auto& target =
                    *static_cast<ayt::math::FVector4*>(targetValue);
                const float next = static_cast<float>(parsed);
                assigned = target[elementIndex] != next;
                if (assigned) target[elementIndex] = next;
            } else if (isReflectedType<ayt::math::FQuaternion>(fieldType)
                       && elementIndex >= 0 && elementIndex < 3) {
                constexpr float degreesToRadians = 0.017453292519943295f;
                auto& target =
                    *static_cast<ayt::math::FQuaternion*>(targetValue);
                auto euler = target.toEulerAngles();
                euler[elementIndex] =
                    static_cast<float>(parsed) * degreesToRadians;
                target = ayt::math::FQuaternion::fromEulerAngles(euler);
                assigned = true;
            } else if (isReflectedType<std::int8_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::int8_t>(targetValue, parsed)
                    : assignIntegralText<std::int8_t>(targetValue, text);
            } else if (isReflectedType<std::uint8_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::uint8_t>(targetValue, parsed)
                    : assignIntegralText<std::uint8_t>(targetValue, text);
            } else if (isReflectedType<std::int16_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::int16_t>(targetValue, parsed)
                    : assignIntegralText<std::int16_t>(targetValue, text);
            } else if (isReflectedType<std::uint16_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::uint16_t>(targetValue, parsed)
                    : assignIntegralText<std::uint16_t>(targetValue, text);
            } else if (isReflectedType<std::int32_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::int32_t>(targetValue, parsed)
                    : assignIntegralText<std::int32_t>(targetValue, text);
            } else if (isReflectedType<std::uint32_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::uint32_t>(targetValue, parsed)
                    : assignIntegralText<std::uint32_t>(targetValue, text);
            } else if (isReflectedType<std::int64_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::int64_t>(targetValue, parsed)
                    : assignIntegralText<std::int64_t>(targetValue, text);
            } else if (isReflectedType<std::uint64_t>(fieldType)) {
                assigned = constrainedIntegral
                    ? assignIntegralValue<std::uint64_t>(targetValue, parsed)
                    : assignIntegralText<std::uint64_t>(targetValue, text);
            }
            return assigned;
        });
    if (!changed) return;
}

void EditorSession::commitInspectorBoolField(
    const std::string& componentType,
    const std::string& fieldName,
    bool checked)
{
    if (_gameView.mode() != EditorMode::Edit || _document == nullptr) return;
    ayt::entity::Entity* entity = _selection.resolve(hierarchyWorldMutable());
    const auto* descriptor = ayt::entity::ComponentRegistry::instance().find(
        componentType);
    ayt::entity::IComponent* component = entity != nullptr
        && descriptor != nullptr && descriptor->get != nullptr
        ? descriptor->get(*entity) : nullptr;
    auto* type = descriptor != nullptr
        ? ayt::reflect::TypeRegistryImpl::instance().findType(
            descriptor->type.hash_code()) : nullptr;
    auto* field = type != nullptr ? type->findField(fieldName.c_str()) : nullptr;
    if (component == nullptr || field == nullptr
        || !isReflectedType<bool>(field->getType())
        || field->hasAttribute(ayt::reflect::FieldAttribute::Hidden)
        || field->hasAttribute(
            ayt::reflect::FieldAttribute::BlueprintReadOnly)) {
        return;
    }
    auto* target = static_cast<bool*>(field->get(component));
    if (target == nullptr || *target == checked) return;
    (void)_document->mutateComponent(
        entity->getId(), componentType, "Edit " + fieldName,
        "property:" + std::to_string(entity->getId()) + ":"
            + componentType + ":" + fieldName,
        [field, checked](ayt::entity::IComponent& editable) {
            auto* value = static_cast<bool*>(field->get(&editable));
            if (value == nullptr || *value == checked) return false;
            *value = checked;
            return true;
        });
}

void EditorSession::refreshTransformInspector()
{
    if (_updatingComponentPropertyCommit) return;
    if (_componentPropertyBody == nullptr
        || _inspectedComponentTypeName != "Transform") {
        return;
    }
    ayt::entity::Entity* entity = _selection.resolve(hierarchyWorldMutable());
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) return;

    auto updateRow = [this](const char* id,
                            const float* values,
                            std::size_t valueCount) {
        ayt::ui::Widget* row = findDescendantById(_componentPropertyBody, id);
        if (row == nullptr) return false;
        const bool readOnly = _gameView.mode() != EditorMode::Edit;
        for (std::size_t index = 0; index < valueCount; ++index) {
            auto* input = dynamic_cast<ayt::ui::TextInput*>(
                findDescendantById(
                    row, std::string(id) + "_" + std::to_string(index)));
            if (input == nullptr) return false;
            if (auto* number = dynamic_cast<InspectorNumberInput*>(input)) {
                number->setInspectorText(formatPreciseFloat(values[index]));
            } else {
                input->setText(formatFloat(values[index]));
            }
            input->setReadOnly(readOnly);
        }
        return true;
    };

    constexpr float radiansToDegrees = 57.29577951308232f;
    const ayt::math::FVector3 euler = transform->rotation.toEulerAngles();
    const float position[3] = {
        transform->position.x, transform->position.y, transform->position.z};
    const float rotation[3] = {
        euler.x * radiansToDegrees,
        euler.y * radiansToDegrees,
        euler.z * radiansToDegrees};
    const float scale[3] = {
        transform->scale.x, transform->scale.y, transform->scale.z};
    (void)updateRow("inspector_field_position", position, 3);
    (void)updateRow("inspector_field_rotation", rotation, 3);
    (void)updateRow("inspector_field_scale", scale, 3);
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

    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    if (!viewportRay(x, y, origin, direction)) {
        return nullptr;
    }

    if (_sceneCamera.isTwoD()) {
        if (std::fabs(direction.z) <= 1.0e-7f) return nullptr;
        const float planeDistance = -origin.z / direction.z;
        const ayt::math::FVector3 worldPoint =
            origin + direction * planeDistance;
        const float unitsPerPixel = _sceneCamera.twoDViewHeight()
            / std::max(viewport.height(), 1.0f);
        const float cameraTolerance = unitsPerPixel * 6.0f;
        const float viewportAspect = viewport.width()
            / std::max(viewport.height(), 1.0f);

        ayt::entity::Entity* best = nullptr;
        Editor2DSelectionShape bestShape{};
        for (ayt::entity::Entity* entity : world->getAllEntities()) {
            auto* transform = entity != nullptr
                ? entity->getComponent<ayt::entity::Transform>() : nullptr;
            if (transform == nullptr) continue;
            Editor2DSelectionShape shape;
            if (!editor2DSelectionShape(*entity, _assetDatabase,
                                        viewportAspect, shape)) {
                continue;
            }
            if (!shape.ignoreTransformScale
                && (std::fabs(transform->scale.x) <= 1.0e-7f
                    || std::fabs(transform->scale.y) <= 1.0e-7f)) {
                continue;
            }
            const ayt::math::Float4x4 inverse =
                editor2DShapeMatrix(*transform, shape).inverse_fast();
            const ayt::math::FVector3 localPoint =
                inverse.transformPoint(worldPoint);
            if (!pointHitsEditor2DShape(shape, localPoint, cameraTolerance)) {
                continue;
            }
            const bool better = best == nullptr
                || shape.priority > bestShape.priority
                || (shape.priority == bestShape.priority
                    && (shape.layer > bestShape.layer
                        || (shape.layer == bestShape.layer
                            && (shape.sortingKey > bestShape.sortingKey
                                || (shape.sortingKey == bestShape.sortingKey
                                    && entity->getId() > best->getId())))));
            if (better) {
                best = entity;
                bestShape = shape;
            }
        }
        return best;
    }

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
    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    if (!viewportRay(x, y, origin, direction)) {
        return EditorGizmoHandle::None;
    }
    const EditorTransformState state{
        transform->position, transform->rotation, transform->scale};
    const float worldScaleOverride = _sceneCamera.isTwoD()
        ? _sceneCamera.twoDViewHeight()
            / static_cast<float>(std::max(1u, _sceneCamera.viewportHeight()))
            * kTwoDGizmoSizePixels
        : 0.0f;
    _gizmoDisabledHandleMask = _sceneCamera.isTwoD()
        ? kTwoDGizmoDisabledHandleMask
        : EditorTransformGizmo::disabledHandleMask(
              state, _localTransformSpace, origin,
              _gizmoDisabledHandleMask);
    return _transformGizmo.hitTestUniversal(
        state, _localTransformSpace, origin, direction,
        _gizmoDisabledHandleMask, worldScaleOverride);
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
    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    if (!viewportRay(x, y, origin, direction)) return false;

    const EditorTransformState state{
        transform->position, transform->rotation, transform->scale};
    const float worldScaleOverride = _sceneCamera.isTwoD()
        ? _sceneCamera.twoDViewHeight()
            / static_cast<float>(std::max(1u, _sceneCamera.viewportHeight()))
            * kTwoDGizmoSizePixels
        : 0.0f;
    if (!_transformGizmo.beginUniversal(
            handle, state, _localTransformSpace,
            origin, direction, y,
            _gizmoDisabledHandleMask, worldScaleOverride)) {
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
    ayt::math::FVector3 origin{};
    ayt::math::FVector3 direction{};
    if (!viewportRay(x, y, origin, direction)) return true;

    EditorTransformState updated;
    if (_transformGizmo.update(
            origin, direction, x, y, updated)) {
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
        _document->executeTransform(entityId, after, "Transform Entity");
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
    ayt::render::EditorSelectionOutline2DState outline;
    if (_gameView.mode() == EditorMode::Edit) {
        ayt::entity::World* world = hierarchyWorldMutable();
        ayt::entity::Entity* entity = _selection.resolve(world);
        auto* transform = entity != nullptr
            ? entity->getComponent<ayt::entity::Transform>() : nullptr;
        if (world != nullptr && world == _selectionWorld
            && transform != nullptr) {
            const EditorTransformState transformState{
                transform->position, transform->rotation, transform->scale};
            if (_sceneCamera.isTwoD()) {
                _gizmoDisabledHandleMask = kTwoDGizmoDisabledHandleMask;
                state.worldScaleOverride = _sceneCamera.twoDViewHeight()
                    / static_cast<float>(std::max(
                        1u, _sceneCamera.viewportHeight()))
                    * kTwoDGizmoSizePixels;
            } else {
                _gizmoDisabledHandleMask =
                    EditorTransformGizmo::disabledHandleMask(
                        transformState, _localTransformSpace,
                        _sceneCamera.threeD().eye(),
                        _gizmoDisabledHandleMask);
            }
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

            if (_sceneCamera.isTwoD()) {
                Editor2DSelectionShape shape;
                const float aspect = static_cast<float>(
                    std::max(1u, _sceneCamera.viewportWidth()))
                    / static_cast<float>(
                        std::max(1u, _sceneCamera.viewportHeight()));
                if (editor2DSelectionShape(*entity, _assetDatabase,
                                           aspect, shape)) {
                    const ayt::math::Float4x4 matrix =
                        editor2DShapeMatrix(*transform, shape);
                    const ayt::math::FVector2 localCorners[4] = {
                        {shape.localMin.x, shape.localMin.y},
                        {shape.localMax.x, shape.localMin.y},
                        {shape.localMax.x, shape.localMax.y},
                        {shape.localMin.x, shape.localMax.y},
                    };
                    for (int index = 0; index < 4; ++index) {
                        outline.corners[index] = matrix.transformPoint({
                            localCorners[index].x,
                            localCorners[index].y, 0.0f});
                    }
                    outline.visible = true;
                    outline.lineWidthWorld = _sceneCamera.twoDViewHeight()
                        / static_cast<float>(std::max(
                            1u, _sceneCamera.viewportHeight())) * 2.0f;
                }
            }
        }
    }
    if (!state.visible) _gizmoDisabledHandleMask = 0u;
    subsystem->renderer().setEditorTransformGizmoState(state);
    subsystem->renderer().setEditorSelectionOutline2DState(outline);
}

// Select the live character for inspection and fall back to the procedural
// cube when character spawn failed.
void EditorSession::selectCharacter()
{
    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = character != nullptr ? character : cube;

    if (e == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] nothing to select; enter Play first "
            "(character or cube)\n");
        refreshInspectorLabels();
        return;
    }

    setSelectedEntity(hierarchyWorldMutable(), e);
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

// Update the Inspector selection/mode hint.
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
    if (_sceneCamera.threeD().isLooking()) {
        _sceneCamera.threeD().endLook();
    }

    ayt::entity::World* activeWorld = hierarchyWorldMutable();
    if (mode == EditorMode::Edit) {
        // Scene history belongs to the persistent Edit document and never
        // targets the transient Play World. Only the Play selection expires.
        clearSelectedEntity(false);
    } else if (_selectionWorld != activeWorld) {
        // Entering Play swaps from the persistent Edit world to its clone.
        // The Edit world is still alive, so its outline can be removed.
        clearSelectedEntity();
    }

    switch (mode) {
    case EditorMode::Edit:
        setModeLabel(L"EDIT");
        setInspectorHint(L"No entity selected");
        pushSceneCameraToRenderer();
        break;
    case EditorMode::Play:
        setModeLabel(_netClientAutoPlay ? L"PLAY (NET CLIENT)" : L"PLAY");
        setInspectorHint(L"Locked during Play.");
        applyRenderSettingsFromPanel();
        pushSceneCameraToRenderer();
        // Auto-select the initial runtime subject, but preserve a viewport
        // selection when resuming Play from Paused.
        if (_selection.empty()) selectCharacter();
        break;
    case EditorMode::Paused:
        setModeLabel(L"PAUSED");
        setInspectorHint(L"Locked during Play.");
        applyRenderSettingsFromPanel();
        pushSceneCameraToRenderer();
        break;
    }

    syncDocumentCommandMenu();
    syncSceneVisibilityMenu();

    // v0.3+ PR-5 — mode 切换会换 Hierarchy 的 World 源（决策 1b）。
    // 选择在上方按 World 生命期切换；这里只排队重建。延迟到 update() 消费
    // 是因为 btn_play/btn_stop click handler 仍在 UIManager 事件派发栈内
    // （Landmine B）。
    _outlinerRefreshPending = true;

    // v0.3 PR-4 — mode 变化时同步 refresh lbl_unsaved（design §4.3.x 决策 5a）
    refreshUnsavedIndicator();
    refreshInspectorLabels();

    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();
    pushSceneCameraToRenderer();
    syncTransformGizmoToRenderer();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::syncViewportIfChanged() {
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
    _sceneCamera.setViewport(
        static_cast<std::uint32_t>(std::max(1.0f, std::round(bounds.width()))),
        static_cast<std::uint32_t>(std::max(1.0f, std::round(bounds.height()))));
    _playRuntime.syncViewportRect(bounds);
    syncSceneUiPreviewBounds();
    pushSceneCameraToRenderer();
}

void EditorSession::syncViewport() {
    _viewportBoundsCached = false;
    syncViewportIfChanged();
}

} // namespace ayt::editor
