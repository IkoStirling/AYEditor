#include "AYEditor/EditorSkeletonExtension.h"

#include "AYEditor/EditorSkeletonDocument.h"
#include "AYEditorSkeletonCanvas.h"

#include <AYAnimation/HumanoidSkeleton.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ListView.h>
#include <AYUI/Slider.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <iomanip>
#include <memory>
#include <sstream>

namespace ayt::editor {
namespace {

using ayt::anim::HumanoidBone;
using ayt::anim::HumanoidBoneRequirement;
using ayt::anim::editor::SkeletonAdaptationState;
using ayt::anim::editor::SkeletonBakeState;
using ayt::anim::editor::SkeletonEditorCore;

std::string encodeUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    std::string result;
    result.reserve(value.size());
    for (wchar_t raw : value) {
        const std::uint32_t code = static_cast<std::uint32_t>(raw);
        if (code <= 0x7fu) result.push_back(static_cast<char>(code));
        else if (code <= 0x7ffu) {
            result.push_back(static_cast<char>(0xc0u | (code >> 6u)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        } else {
            result.push_back(static_cast<char>(0xe0u | (code >> 12u)));
            result.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
    }
    return result;
}

ayt::math::FVector4 adaptationColor(SkeletonAdaptationState state)
{
    if (state == SkeletonAdaptationState::Validated
        || state == SkeletonAdaptationState::Native) {
        return {0.35f, 0.82f, 0.50f, 1.0f};
    }
    if (state == SkeletonAdaptationState::Incomplete) {
        return {0.96f, 0.58f, 0.24f, 1.0f};
    }
    return {0.62f, 0.66f, 0.74f, 1.0f};
}

ayt::math::FVector4 bakeColor(SkeletonBakeState state)
{
    if (state == SkeletonBakeState::Ready) return {0.35f, 0.82f, 0.50f, 1.0f};
    if (state == SkeletonBakeState::Failed) return {0.95f, 0.34f, 0.34f, 1.0f};
    if (state == SkeletonBakeState::Stale) return {0.96f, 0.58f, 0.24f, 1.0f};
    return {0.62f, 0.66f, 0.74f, 1.0f};
}

std::wstring stateLabel(const wchar_t* prefix, const char* state)
{
    return std::wstring(prefix) + ayt::ui::decodeUtf8Text(state);
}

class EditorSkeletonWorkspaceView final : public IEditorView {
public:
    EditorSkeletonWorkspaceView(std::shared_ptr<EditorSkeletonDocument> document,
                                IEditorHostServices& host)
        : _document(std::move(document)), _host(host)
    {
        build();
        refreshAll();
    }

    ~EditorSkeletonWorkspaceView() override
    {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        ayt::ui::Widget* root = _root;
        _root = nullptr;
        return root;
    }
    IEditorCommandTarget* commandTarget() noexcept override {
        return _document.get();
    }
    void tick(float dt) override
    {
        if (_document == nullptr) return;
        if (_document->timelinePlaying()) {
            _document->timelineTick(dt);
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
            _host.requestRepaint();
        }
        if (_lastRevision != _document->revision()) {
            _lastRevision = _document->revision();
            refreshStatus();
            refreshPreflight();
        }
    }
    bool wantsBackgroundTick() const noexcept override {
        return _document != nullptr && _document->timelinePlaying();
    }

private:
    ayt::ui::Button* makeButton(const wchar_t* text,
                                std::function<void()> callback)
    {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setPadding(7.0f, 3.0f, 7.0f, 3.0f);
        button->setOnClicked(std::move(callback));
        return button;
    }

    ayt::ui::TextLabel* makeHeader(const wchar_t* text)
    {
        auto* label = new ayt::ui::TextLabel();
        label->setText(text);
        label->setFontSize(11);
        label->setTextColor({0.58f, 0.63f, 0.72f, 1.0f});
        label->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        return label;
    }

    void build()
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(4.0f);
        root->setPadding(5.0f, 5.0f, 5.0f, 5.0f);

        auto* toolbar = new ayt::ui::HBox();
        toolbar->setSpacing(5.0f);
        toolbar->addWidget(makeButton(L"Save", [this]() { save(); }), 54.0f);
        toolbar->addWidget(makeButton(L"Undo", [this]() {
            if (_document->core().undo()) refreshAll();
        }), 52.0f);
        toolbar->addWidget(makeButton(L"Redo", [this]() {
            if (_document->core().redo()) refreshAll();
        }), 52.0f);
        toolbar->addWidget(makeButton(L"Frame", [this]() {
            if (_canvas != nullptr) _canvas->frameSkeleton();
        }), 58.0f);
        toolbar->addWidget(makeButton(L"Check", [this]() {
            refreshPreflight();
            _host.setStatusText(_document->core().preflight().canBake()
                ? L"Skeleton preflight passed"
                : L"Skeleton preflight found blocking issues");
        }), 58.0f);
        _adaptation = new ayt::ui::TextLabel();
        _adaptation->setFontSize(11);
        _adaptation->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        _bake = new ayt::ui::TextLabel();
        _bake->setFontSize(11);
        _bake->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        toolbar->addWidget(_adaptation, 150.0f);
        toolbar->addWidget(_bake, 120.0f);
        _status = new ayt::ui::TextLabel();
        _status->setFontSize(11);
        _status->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        toolbar->addWidget(_status, 0.0f);
        root->addWidget(toolbar, 28.0f);

        auto* body = new ayt::ui::HBox();
        body->setSpacing(5.0f);

        auto* hierarchy = new ayt::ui::VBox();
        hierarchy->setSpacing(3.0f);
        hierarchy->addWidget(makeHeader(L"SKELETON HIERARCHY"), 20.0f);
        _boneList = new ayt::ui::ListView();
        _boneList->setItemHeight(20.0f);
        _boneList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0) return;
            (void)_document->core().selectBone(index);
            refreshBoneProperties();
            if (_canvas != nullptr) _canvas->markDirty();
        });
        hierarchy->addWidget(_boneList, 0.0f);
        body->addWidget(hierarchy, 230.0f);

        _canvas = new EditorSkeletonCanvas(_document);
        _canvas->setOnBoneSelected([this](int index) {
            _syncing = true;
            _boneList->setSelectedIndex(index);
            _syncing = false;
            refreshBoneProperties();
        });
        body->addWidget(_canvas, 0.0f);

        auto* inspector = new ayt::ui::VBox();
        inspector->setSpacing(3.0f);
        inspector->addWidget(makeHeader(L"BONE"), 20.0f);
        _properties = new ayt::ui::TextArea();
        _properties->setReadOnly(true);
        _properties->setWordWrap(false);
        inspector->addWidget(_properties, 124.0f);
        inspector->addWidget(makeHeader(L"AYHUMANOID MAPPING"), 20.0f);
        _roleList = new ayt::ui::ListView();
        _roleList->setItemHeight(20.0f);
        _roleList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || static_cast<std::size_t>(index) >= ayt::anim::kHumanoidBoneCount) {
                return;
            }
            (void)_document->core().selectRole(static_cast<HumanoidBone>(index));
            refreshRolePicker();
        });
        inspector->addWidget(_roleList, 0.0f);
        _bonePicker = new ayt::ui::ComboBox();
        _bonePicker->setMaxPopupItems(12);
        _bonePicker->setOnSelectionChanged([this](int index) {
            if (_syncing) return;
            const HumanoidBone role = _document->core().selectedRole();
            if (index <= 0) (void)_document->core().unbind(role);
            else (void)_document->core().bind(role, index - 1);
            refreshMapping();
        });
        inspector->addWidget(_bonePicker, 28.0f);
        auto* mappingActions = new ayt::ui::HBox();
        mappingActions->setSpacing(4.0f);
        mappingActions->addWidget(makeButton(L"Canonical template", [this]() {
            (void)_document->core().applyCanonicalNameTemplate();
            refreshAll();
        }), 132.0f);
        mappingActions->addWidget(makeButton(L"Clear", [this]() {
            (void)_document->core().clearMapping();
            refreshAll();
        }), 52.0f);
        _nativeButton = makeButton(L"Native: Off", [this]() {
            (void)_document->core().setNative(!_document->core().isNative());
            refreshAll();
        });
        mappingActions->addWidget(_nativeButton, 86.0f);
        inspector->addWidget(mappingActions, 28.0f);
        body->addWidget(inspector, 330.0f);
        root->addWidget(body, 0.0f);

        _diagnostics = new ayt::ui::TextArea();
        _diagnostics->setReadOnly(true);
        _diagnostics->setWordWrap(false);
        root->addWidget(_diagnostics, 78.0f);

        auto* animation = new ayt::ui::HBox();
        animation->setSpacing(4.0f);
        animation->addWidget(makeHeader(L"ANIMATION"), 76.0f);
        _animationPath = new ayt::ui::TextInput();
        _animationPath->setText(L"");
        animation->addWidget(_animationPath, 0.0f);
        animation->addWidget(makeButton(L"Load", [this]() { loadAnimation(); }), 48.0f);
        animation->addWidget(makeButton(L"Play", [this]() {
            _document->timelinePlay(); refreshTransport();
        }), 48.0f);
        animation->addWidget(makeButton(L"Pause", [this]() {
            _document->timelinePause(); refreshTransport();
        }), 54.0f);
        animation->addWidget(makeButton(L"Stop", [this]() {
            _document->timelineStop(); refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
        }), 48.0f);
        _timeline = new ayt::ui::Slider();
        _timeline->setValueRange(0.0f, 1.0f);
        _timeline->setOnValueChanged([this](float value) {
            if (_syncing) return;
            (void)_document->setTimelinePositionSeconds(value);
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
        });
        animation->addWidget(_timeline, 210.0f);
        _time = new ayt::ui::TextLabel();
        _time->setFontSize(11);
        _time->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        animation->addWidget(_time, 92.0f);
        root->addWidget(animation, 30.0f);
    }

    void refreshAll()
    {
        refreshHierarchy();
        refreshMapping();
        refreshBoneProperties();
        refreshStatus();
        refreshPreflight();
        refreshTransport();
        _lastRevision = _document->revision();
        _host.requestRepaint();
    }

    void refreshHierarchy()
    {
        std::vector<std::wstring> items;
        for (const auto& bone : _document->core().bones()) {
            std::wstring label(static_cast<std::size_t>(bone.depth * 2), L' ');
            label += ayt::ui::decodeUtf8Text(bone.name);
            label += L"  [" + std::to_wstring(bone.index) + L"]";
            items.push_back(std::move(label));
        }
        _syncing = true;
        _boneList->setItems(items);
        _boneList->setSelectedIndex(_document->core().selectedBone());
        std::vector<std::wstring> choices{L"<Unmapped>"};
        for (const auto& bone : _document->core().bones()) {
            choices.push_back(ayt::ui::decodeUtf8Text(bone.name)
                + L"  [" + std::to_wstring(bone.index) + L"]");
        }
        _bonePicker->setItems(choices);
        _syncing = false;
    }

    void refreshMapping()
    {
        std::vector<std::wstring> roles;
        roles.reserve(ayt::anim::kHumanoidBoneCount);
        for (const auto& spec : ayt::anim::getHumanoidBoneSpecs()) {
            std::wstring label = spec.requirement == HumanoidBoneRequirement::Required
                ? L"* " : L"  ";
            label += ayt::ui::decodeUtf8Text(std::string(spec.canonicalName));
            const int bone = _document->core().mapping().getSourceBoneIndex(spec.role);
            if (bone >= 0 && bone < static_cast<int>(_document->core().bones().size())) {
                label += L"  ->  " + ayt::ui::decodeUtf8Text(
                    _document->core().bones()[bone].name);
            }
            roles.push_back(std::move(label));
        }
        _syncing = true;
        _roleList->setItems(roles);
        _roleList->setSelectedIndex(
            static_cast<int>(_document->core().selectedRole()));
        _nativeButton->setText(_document->core().isNative()
            ? L"Native: On" : L"Native: Off");
        _syncing = false;
        refreshRolePicker();
        refreshStatus();
    }

    void refreshRolePicker()
    {
        const int mapped = _document->core().mapping().getSourceBoneIndex(
            _document->core().selectedRole());
        _syncing = true;
        _bonePicker->setSelectedIndex(mapped >= 0 ? mapped + 1 : 0);
        _syncing = false;
    }

    void refreshBoneProperties()
    {
        const int index = _document->core().selectedBone();
        if (index < 0 || index >= static_cast<int>(_document->core().bones().size())) {
            _properties->setText(L"No bone selected.");
            return;
        }
        const auto& bone = _document->core().bones()[index];
        std::wostringstream text;
        text << L"Name: " << ayt::ui::decodeUtf8Text(bone.name)
             << L"\nIndex: " << bone.index << L"\nParent: " << bone.parentIndex
             << std::fixed << std::setprecision(3)
             << L"\nPosition: " << bone.localPosition.x << L", "
             << bone.localPosition.y << L", " << bone.localPosition.z
             << L"\nRotation: " << bone.localRotation.x << L", "
             << bone.localRotation.y << L", " << bone.localRotation.z
             << L", " << bone.localRotation.w
             << L"\nScale: " << bone.localScale.x << L", "
             << bone.localScale.y << L", " << bone.localScale.z;
        _properties->setText(text.str());
    }

    void refreshStatus()
    {
        const auto status = _document->core().status();
        _adaptation->setText(stateLabel(L"Mapping: ",
            SkeletonEditorCore::adaptationStateName(status.adaptation)));
        _adaptation->setTextColor(adaptationColor(status.adaptation));
        _bake->setText(stateLabel(L"Bake: ",
            SkeletonEditorCore::bakeStateName(status.bake)));
        _bake->setTextColor(bakeColor(status.bake));
        _status->setText((_document->isDirty() ? L"Modified | " : L"")
            + ayt::ui::decodeUtf8Text(status.message));
        _status->setTextColor(_document->isDirty()
            ? ayt::math::FVector4{0.95f, 0.72f, 0.30f, 1.0f}
            : ayt::math::FVector4{0.62f, 0.66f, 0.74f, 1.0f});
    }

    void refreshPreflight()
    {
        if (_diagnostics == nullptr) return;
        const auto report = _document->core().preflight();
        std::wostringstream text;
        if (report.canBake()) {
            text << L"PREFLIGHT PASSED";
        } else {
            text << L"PREFLIGHT BLOCKED  |  " << report.errorCount()
                 << L" error(s), " << report.warningCount() << L" warning(s)";
        }
        const std::size_t visible = std::min<std::size_t>(report.issues.size(), 6u);
        for (std::size_t index = 0; index < visible; ++index) {
            const auto& issue = report.issues[index];
            text << L"\n[" << ayt::ui::decodeUtf8Text(
                SkeletonEditorCore::preflightCodeName(issue.code)) << L"] "
                 << ayt::ui::decodeUtf8Text(issue.message);
        }
        if (report.issues.size() > visible) {
            text << L"\n... " << (report.issues.size() - visible)
                 << L" more issue(s)";
        }
        _diagnostics->setText(text.str());
    }

    void refreshTransport()
    {
        _syncing = true;
        const float duration = static_cast<float>(_document->timelineDurationSeconds());
        _timeline->setValueRange(0.0f, std::max(duration, 0.001f));
        _timeline->setValue(static_cast<float>(_document->timelinePositionSeconds()));
        _syncing = false;
        std::wostringstream text;
        text << std::fixed << std::setprecision(2)
             << _document->timelinePositionSeconds() << L" / "
             << _document->timelineDurationSeconds();
        _time->setText(text.str());
    }

    void save()
    {
        std::string error;
        if (_document->save(&error)) {
            _host.setStatusText(L"Skeleton mapping saved");
        } else {
            _host.setStatusText(L"Skeleton mapping save failed: "
                + ayt::ui::decodeUtf8Text(error));
        }
        refreshStatus();
    }

    void loadAnimation()
    {
        std::string error;
        if (!_document->core().attachAnimation(
                encodeUtf8(_animationPath->getText()), &error)) {
            _host.setStatusText(L"Animation load failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        _host.setStatusText(L"Animation attached to skeleton preview");
        refreshTransport();
        if (_canvas != nullptr) _canvas->markDirty();
    }

    std::shared_ptr<EditorSkeletonDocument> _document;
    IEditorHostServices& _host;
    ayt::ui::Widget* _root = nullptr;
    EditorSkeletonCanvas* _canvas = nullptr;
    ayt::ui::ListView* _boneList = nullptr;
    ayt::ui::ListView* _roleList = nullptr;
    ayt::ui::ComboBox* _bonePicker = nullptr;
    ayt::ui::TextArea* _properties = nullptr;
    ayt::ui::TextArea* _diagnostics = nullptr;
    ayt::ui::TextInput* _animationPath = nullptr;
    ayt::ui::Slider* _timeline = nullptr;
    ayt::ui::TextLabel* _time = nullptr;
    ayt::ui::TextLabel* _adaptation = nullptr;
    ayt::ui::TextLabel* _bake = nullptr;
    ayt::ui::TextLabel* _status = nullptr;
    ayt::ui::Button* _nativeButton = nullptr;
    std::uint64_t _lastRevision = 0u;
    bool _syncing = false;
};

} // namespace

EditorDescriptor makeEditorSkeletonDescriptor()
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorSkeletonExtensionId;
    descriptor.displayName = L"Skeleton Editor";
    descriptor.iconPath = "icons/outline/accessibility.svg";
    descriptor.surfaceKind = EditorSurfaceKind::Document;
    descriptor.openPolicy = EditorOpenPolicy::PerResource;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.priority = 130;
    descriptor.extensions = {".ayskel", ".aysmap"};
    descriptor.assetTypes = {"Skeleton", "Skeleton Mapping"};
    descriptor.createDocument = [](const EditorOpenRequest& request,
                                   std::string& error) {
        auto document = std::make_shared<EditorSkeletonDocument>();
        return document->initialize(request, error)
            ? std::static_pointer_cast<IEditorDocument>(document) : nullptr;
    };
    descriptor.createView = [](
        const std::shared_ptr<IEditorDocument>& document,
        IEditorHostServices& host) -> std::unique_ptr<IEditorView> {
        auto skeleton = std::dynamic_pointer_cast<EditorSkeletonDocument>(document);
        if (skeleton == nullptr) return nullptr;
        return std::make_unique<EditorSkeletonWorkspaceView>(
            std::move(skeleton), host);
    };
    return descriptor;
}

bool registerEditorSkeletonExtension(EditorExtensionRegistry& registry,
                                     std::string* error)
{
    return registry.registerEditor(makeEditorSkeletonDescriptor(), error);
}

} // namespace ayt::editor
