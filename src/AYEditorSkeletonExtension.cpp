#include "AYEditor/EditorSkeletonExtension.h"
#include <AYUI/Authoring/DragSourceList.h>
#include <AYUI/Authoring/StableListRows.h>

#include "AYEditor/EditorSkeletonDocument.h"
#include "AYEditorSkeletonCanvas.h"
#include <AYEditor/EditorCommandButtons.h>
#include <AYEditor/EditorAuthoringSelectionBridge.h>
#include <AYUI/Authoring/DiagnosticsPanel.h>
#include <AYUI/Authoring/ResourceReferenceField.h>
#include "AYEditorTimelinePlaybackSource.h"
#include <AYUI/Authoring/AuthoringPrimitives.h>

#include <AYAnimation/HumanoidSkeleton.h>
#include <AYAnimationEditor/SkeletonBakeJob.h>
#include <AYUI/Authoring/JobPresentation.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ListView.h>
#include <AYUI/Slider.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <cwctype>
#include <memory>
#include <sstream>

namespace ayt::editor {
namespace {

using ayt::anim::HumanoidBone;
using ayt::anim::HumanoidBoneRequirement;
using ayt::anim::editor::SkeletonAdaptationState;
using ayt::anim::editor::SkeletonBakeState;
using ayt::anim::editor::SkeletonEditorCore;

std::string encodeUtf8(const std::wstring& text)
{
    return ayt::ui::encodeUtf8Text(text);
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
    if (state == SkeletonAdaptationState::Invalid) {
        return {0.95f, 0.34f, 0.34f, 1.0f};
    }
    return {0.62f, 0.66f, 0.74f, 1.0f};
}

ayt::math::FVector4 bakeColor(SkeletonBakeState state)
{
    if (state == SkeletonBakeState::Current) return {0.35f, 0.82f, 0.50f, 1.0f};
    if (state == SkeletonBakeState::Baking) return {0.36f, 0.68f, 0.96f, 1.0f};
    if (state == SkeletonBakeState::Failed) return {0.95f, 0.34f, 0.34f, 1.0f};
    if (state == SkeletonBakeState::Stale) return {0.96f, 0.58f, 0.24f, 1.0f};
    return {0.62f, 0.66f, 0.74f, 1.0f};
}

std::wstring stateLabel(const wchar_t* prefix, const char* state)
{
    return std::wstring(prefix) + ayt::ui::decodeUtf8Text(state);
}

std::wstring lowerText(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) { return std::towlower(character); });
    return value;
}

std::wstring quaternionText(const ayt::math::FQuaternion& value)
{
    std::wostringstream text;
    text << std::fixed << std::setprecision(4) << value.x << L", "
         << value.y << L", " << value.z << L", " << value.w;
    return text.str();
}

bool parseQuaternionText(std::wstring text, ayt::math::FQuaternion& value)
{
    std::replace(text.begin(), text.end(), L',', L' ');
    std::wistringstream input(text);
    return static_cast<bool>(input >> value.x >> value.y >> value.z >> value.w);
}

ayt::ui::authoring::JobStatusSnapshot bakePresentationSnapshot(
    const ayt::anim::editor::SkeletonBakeJobSnapshot& bake)
{
    using namespace ayt::ui::authoring;
    using S = ayt::anim::editor::SkeletonBakeJobState;
    JobStatusSnapshot result;
    result.generation = bake.generation;
    switch (bake.state) {
    case S::Idle: result.state = JobState::Idle; break;
    case S::Running: result.state = JobState::Running; break;
    case S::Succeeded: result.state = JobState::Succeeded; break;
    case S::Failed: result.state = JobState::Failed; break;
    case S::Cancelled: result.state = JobState::Cancelled; break;
    }
    result.progress = bake.progress;
    result.message = ayt::ui::decodeUtf8Text(bake.message);
    for (const auto& path : bake.outputPaths) result.outputs.push_back(ayt::ui::decodeUtf8Text(path));
    result.cancellable = bake.state == S::Running;
    return result;
}

using SkeletonBoneDragList = ayt::ui::authoring::DragSourceList;

class EditorSkeletonWorkspaceView final : public IEditorView, public IEditorCommandTarget {
public:
    EditorSkeletonWorkspaceView(std::shared_ptr<EditorSkeletonDocument> document,
                                IEditorHostServices& host)
        : _document(std::move(document)), _host(host), _commands([this] { return commandTarget(); }),
          _selectionBridge(host.workspace(), _document)
    {
        _document->configureProjectRoot(_host.projectRoot());
        build();
        refreshAll();
    }

    ~EditorSkeletonWorkspaceView() override
    {
        _commands.detach();
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        ayt::ui::Widget* root = _root;
        _root = nullptr;
        return root;
    }
    void prepareForUiShutdown() override { _commands.detach(); _bakePresentation.reset(); }
    EditorSelectionContext* selectionContext() noexcept override { return _selectionBridge.context(); }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    bool handlesCommand(const std::string& id) const override { return _document && _document->handlesCommand(id); }
    bool canExecuteCommand(const std::string& id) const override { return _document && _document->canExecuteCommand(id); }
    bool executeCommand(const std::string& id) override {
        if (!canExecuteCommand(id)) return false;
        std::string error;
        const bool success = id == "file.save" ? _document->save(&error) : _document->executeCommand(id);
        if (!success) { _host.setStatusText(L"Skeleton command failed: " + ayt::ui::decodeUtf8Text(error)); return false; }
        _host.setStatusText(L"Skeleton command completed"); refreshAll(); return true;
    }
    void tick(float dt) override
    {
        if (_document == nullptr) return;
        _commands.refresh();
        pollBake();
        if (_document->timelinePlaying()) {
            _document->timelineTick(dt);
        }
        const auto changes = _refreshGate.consume(stateStamp());
        if (changes.pose || changes.transport) {
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
            _host.requestRepaint();
        }
        if (changes.content) {
            refreshStatus();
            refreshPreflight();
        }
    }
    bool wantsBackgroundTick() const noexcept override {
        return _document != nullptr && (_document->timelinePlaying()
            || _bakeJob.poll().state
                == ayt::anim::editor::SkeletonBakeJobState::Running);
    }

private:
    ayt::ui::authoring::AuthoringStateStamp stateStamp() const {
        return {_document->revision(), 0u, _document->core().poseRevision(),
                _document->timelinePositionSeconds(), _document->timelinePlaying()};
    }
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
        _commands.add(*toolbar, L"Save", "file.save", 54);
        _commands.add(*toolbar, L"Undo", "edit.undo", 52);
        _commands.add(*toolbar, L"Redo", "edit.redo", 52);
        toolbar->addWidget(makeButton(L"Frame", [this]() {
            if (_canvas != nullptr) _canvas->frameSkeleton();
        }), 58.0f);
        toolbar->addWidget(makeButton(L"Check", [this]() {
            _host.setStatusText(refreshPreflight()
                ? L"Skeleton preflight passed"
                : L"Skeleton preflight found blocking issues");
        }), 58.0f);
        toolbar->addWidget(makeButton(L"Dry Run", [this]() {
            runDryRun();
        }), 66.0f);
        toolbar->addWidget(makeButton(L"Bake", [this]() {
            startBake();
        }), 52.0f);
        toolbar->addWidget(makeButton(L"Cancel", [this]() {
            const auto fresh = bakePresentationSnapshot(_bakeJob.poll());
            (void)_bakePresentation.requestCancel(fresh, [this](std::uint64_t) {
                _bakeJob.cancel();
                return true;
            });
            pollBake();
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
        _boneSearch = new ayt::ui::TextInput();
        _boneSearch->setId("skeleton_bone_search");
        _boneSearch->setPlaceholder(L"Search bone name or index…");
        _boneSearch->setOnTextChanged([this](const std::wstring&) {
            refreshHierarchy();
        });
        hierarchy->addWidget(_boneSearch, 26.0f);
        auto* boneList = new SkeletonBoneDragList();
        _boneList = boneList;
        _boneList->setId("skeleton_bone_list");
        _boneList->setItemHeight(20.0f);
        _boneList->setOnSelectionChanged([this](int index) {
            const auto* selected = _visibleBoneRows.idAt(index);
            if (_syncing || !selected) return;
            const int boneIndex = *selected;
            (void)_document->core().selectBone(boneIndex);
            refreshBoneProperties();
            if (_canvas != nullptr) _canvas->markDirty();
        });
        boneList->setPayloadProvider([this](int row) {
            ayt::ui::DragPayload payload;
            const auto* selected = _visibleBoneRows.idAt(row);
            if (!selected) return payload;
            const int boneIndex = *selected;
            if (boneIndex < 0 || static_cast<std::size_t>(boneIndex) >= _document->core().bones().size()) return payload;
            payload.kind = "SkeletonBone";
            payload.userData = boneIndex;
            payload.text = ayt::ui::decodeUtf8Text(
                _document->core().bones()[static_cast<std::size_t>(boneIndex)].name);
            return payload;
        });
        hierarchy->addWidget(_boneList, 0.0f);
        body->addWidget(hierarchy, 230.0f);

        _canvas = new EditorSkeletonCanvas(_document);
        _canvas->setOnBoneSelected([this](int index) {
            _syncing = true;
            _boneList->setSelectedIndex(_visibleBoneRows.indexOf(index));
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
        auto* targetRow = new ayt::ui::authoring::ResourceReferenceField(
            {L"", L"Target .ayskel path", L"Set target", "skeleton_retarget_target", 0, 78});
        _targetSkeletonPath = targetRow->input();
        targetRow->setOnLoad([this](const auto&) { return configureRetarget(); });
        inspector->addWidget(targetRow, 28.0f);
        auto* platformRow = new ayt::ui::HBox();
        platformRow->setSpacing(4.0f);
        _platform = new ayt::ui::TextInput();
        _platform->setId("skeleton_retarget_platform");
        _platform->setPlaceholder(L"Platform/capability (default)");
        platformRow->addWidget(_platform, 0.0f);
        platformRow->addWidget(makeButton(L"Mapping", [this]() {
            clearRetarget();
        }), 78.0f);
        inspector->addWidget(platformRow, 28.0f);
        auto* profileRow = new ayt::ui::HBox();
        profileRow->setSpacing(4.0f);
        _profilePicker = new ayt::ui::ComboBox();
        _profilePicker->setMaxPopupItems(10);
        profileRow->addWidget(_profilePicker, 0.0f);
        profileRow->addWidget(makeButton(L"Use profile", [this]() {
            switchProfile();
        }), 82.0f);
        inspector->addWidget(profileRow, 28.0f);
        auto* templateRow = new ayt::ui::HBox();
        templateRow->setSpacing(4.0f);
        _templatePicker = new ayt::ui::ComboBox();
        _templatePicker->setMaxPopupItems(10);
        templateRow->addWidget(_templatePicker, 0.0f);
        templateRow->addWidget(makeButton(L"Preview", [this]() {
            previewSelectedTemplate();
        }), 62.0f);
        templateRow->addWidget(makeButton(L"Apply template", [this]() {
            applySelectedTemplate();
        }), 92.0f);
        inspector->addWidget(templateRow, 28.0f);
        _roleList = new ayt::ui::ListView();
        _roleList->setId("skeleton_role_list");
        _roleList->setItemHeight(20.0f);
        _roleList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || static_cast<std::size_t>(index) >= ayt::anim::kHumanoidBoneCount) {
                return;
            }
            (void)_document->core().selectRole(static_cast<HumanoidBone>(index));
            refreshRolePicker();
            refreshTargetRolePicker();
            refreshCorrectionEditor();
        });
        _roleList->setAcceptDrops(true);
        _roleList->setAcceptDropKinds({"SkeletonBone"});
        _roleList->setOnDrop([this](const ayt::ui::DragPayload& payload) {
            if (payload.kind != "SkeletonBone") return;
            const auto* manager = ayt::ui::UIManager::tryGet();
            if (manager == nullptr) return;
            const auto point = manager->getDragLastMousePos();
            const auto bounds = _roleList->getWorldBounds();
            if (!bounds.contains(point)) return;
            const float localY = point.y - bounds.minY
                + _roleList->getScrollOffset().y;
            const int row = static_cast<int>(std::floor(
                localY / (std::max)(1.0f, _roleList->getItemHeight())));
            if (row < 0
                || static_cast<std::size_t>(row) >= ayt::anim::kHumanoidBoneCount) {
                return;
            }
            const auto role = static_cast<HumanoidBone>(row);
            if (!_document->core().selectRole(role)
                || !_document->core().bind(role, payload.userData)) {
                _host.setStatusText(L"Bone mapping drop was rejected");
                return;
            }
            _host.setStatusText(L"Mapped " + payload.text + L" by drag and drop");
            refreshMapping();
            refreshHierarchy();
            refreshPreflight();
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
            refreshHierarchy();
        });
        inspector->addWidget(_bonePicker, 28.0f);
        _targetBonePicker = new ayt::ui::ComboBox();
        _targetBonePicker->setId("skeleton_target_bone_picker");
        _targetBonePicker->setMaxPopupItems(12);
        _targetBonePicker->setOnSelectionChanged([this](int index) {
            if (_syncing || _document->core().profileKind()
                    != ayt::anim::editor::RigProfileKind::Retarget) return;
            const HumanoidBone role = _document->core().selectedRole();
            if (index <= 0) (void)_document->core().unbindTarget(role);
            else (void)_document->core().bindTarget(role, index - 1);
            refreshMapping();
        });
        inspector->addWidget(_targetBonePicker, 28.0f);
        auto* correctionRow = new ayt::ui::HBox();
        correctionRow->setSpacing(4.0f);
        _correctionKind = new ayt::ui::ComboBox();
        _correctionKind->setId("skeleton_correction_kind");
        _correctionKind->setItems({L"Source ref", L"Target ref", L"Axis"});
        _correctionKind->setSelectedIndex(2);
        _correctionKind->setOnSelectionChanged([this](int) {
            if (!_syncing) refreshCorrectionEditor();
        });
        correctionRow->addWidget(_correctionKind, 86.0f);
        _correctionValue = new ayt::ui::TextInput();
        _correctionValue->setId("skeleton_correction_quaternion");
        _correctionValue->setPlaceholder(L"x, y, z, w");
        correctionRow->addWidget(_correctionValue, 0.0f);
        correctionRow->addWidget(makeButton(L"Apply", [this]() {
            applyCorrection();
        }), 48.0f);
        inspector->addWidget(correctionRow, 28.0f);
        auto* mappingActions = new ayt::ui::HBox();
        mappingActions->setSpacing(4.0f);
        mappingActions->addWidget(makeButton(L"Canonical", [this]() {
            (void)_document->core().applyCanonicalNameTemplate();
            refreshAll();
        }), 90.0f);
        mappingActions->addWidget(makeButton(L"Clear", [this]() {
            (void)_document->core().clearMapping();
            refreshAll();
        }), 44.0f);
        _nativeButton = makeButton(L"Native: Off", [this]() {
            (void)_document->core().setNative(!_document->core().isNative());
            refreshAll();
        });
        mappingActions->addWidget(_nativeButton, 72.0f);
        _customButton = makeButton(L"Custom: Off", [this]() {
            (void)_document->core().setNotApplicable(
                !_document->core().isNotApplicable());
            refreshAll();
        });
        mappingActions->addWidget(_customButton, 76.0f);
        inspector->addWidget(mappingActions, 28.0f);
        body->addWidget(inspector, 330.0f);
        root->addWidget(body, 0.0f);

        _diagnostics = new ayt::ui::authoring::DiagnosticsPanel();
        _diagnostics->setId("skeleton_diagnostics");
        _diagnostics->setOnLocate([this](const std::string& target) { locateDiagnostic(target); });
        root->addWidget(_diagnostics, 102.0f);

        auto* animation = new ayt::ui::HBox();
        animation->setSpacing(4.0f);
        animation->addWidget(makeHeader(L"ANIMATION"), 76.0f);
        auto* animationField = new ayt::ui::authoring::ResourceReferenceField();
        _animationPath = animationField->input();
        animationField->setOnLoad([this](const auto&) { return loadAnimation(); });
        animation->addWidget(animationField, 0.0f);
        const auto owner = _document;
        auto playback = std::make_shared<TimelinePlaybackSource>([owner] { return owner; });
        _transport = new ayt::ui::authoring::PlaybackControls(playback,
            {true, true, true, false, false, false, 2});
        _transport->setOnChanged([this] {
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
        });
        animation->addWidget(_transport, 470.0f);
        root->addWidget(animation, 30.0f);
    }

    void refreshAll()
    {
        _commands.refresh();
        refreshHierarchy();
        refreshProfiles();
        refreshMapping();
        refreshBoneProperties();
        refreshStatus();
        refreshPreflight();
        refreshTransport();
        _refreshGate.acknowledge(stateStamp());
        _host.requestRepaint();
    }

    void refreshProfiles()
    {
        std::vector<std::wstring> profiles;
        const auto& availableProfiles = _document->mappingProfiles();
        int selectedProfile = -1;
        for (std::size_t index = 0; index < availableProfiles.size(); ++index) {
            const auto& profile = availableProfiles[index];
            profiles.push_back(L"[" + ayt::ui::decodeUtf8Text(
                    SkeletonEditorCore::rigProfileKindName(profile.kind)) + L"] "
                + ayt::ui::decodeUtf8Text(profile.name)
                + L"  [" + ayt::ui::decodeUtf8Text(
                    std::filesystem::path(profile.path).filename().string()) + L"]");
            std::error_code error;
            if (std::filesystem::equivalent(profile.path,
                    _document->core().mappingPath(), error) && !error) {
                selectedProfile = static_cast<int>(index);
            }
        }
        if (profiles.empty()) profiles.push_back(L"<No saved mapping profiles>");
        _profilePicker->setItems(profiles);
        _profilePicker->setSelectedIndex(
            selectedProfile >= 0 ? selectedProfile : 0);
        _profilePicker->setEnabled(!availableProfiles.empty());
        if (_targetSkeletonPath != nullptr) {
            _targetSkeletonPath->setText(ayt::ui::decodeUtf8Text(
                _document->core().targetSkeletonPath()));
        }
        if (_platform != nullptr) {
            _platform->setText(ayt::ui::decodeUtf8Text(
                _document->core().bakePlatform()));
        }

        std::vector<std::wstring> templates;
        for (const auto& profile : _document->templates()) {
            templates.push_back(ayt::ui::decodeUtf8Text(profile.name)
                + L"  [" + ayt::ui::decodeUtf8Text(
                    std::filesystem::path(profile.path).filename().string()) + L"]");
        }
        if (templates.empty()) templates.push_back(L"<No .ayrig templates>");
        _templatePicker->setItems(templates);
        _templatePicker->setSelectedIndex(0);
        _templatePicker->setEnabled(!_document->templates().empty());
    }

    void switchProfile()
    {
        const int index = _profilePicker->getSelectedIndex();
        const auto& profiles = _document->mappingProfiles();
        if (index < 0 || static_cast<std::size_t>(index) >= profiles.size()) {
            _host.setStatusText(L"No mapping profile selected");
            return;
        }
        std::string error;
        if (!_document->switchMappingProfile(profiles[index].path, &error)) {
            _host.setStatusText(L"Profile switch failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        _host.setStatusText(L"Skeleton mapping profile switched");
        refreshAll();
    }

    ayt::ui::authoring::ResourceReferenceResult configureRetarget()
    {
        std::string error;
        if (!_document->configureRetarget(
                encodeUtf8(_targetSkeletonPath->getText()),
                encodeUtf8(_platform->getText()), &error)) {
            const auto message = L"Retarget target rejected: " + ayt::ui::decodeUtf8Text(error);
            _host.setStatusText(message); return {false, message};
        }
        _host.setStatusText(L"Retarget target configured");
        refreshAll(); return {true, L"Retarget target configured"};
    }

    void clearRetarget()
    {
        (void)_document->clearRetarget();
        _host.setStatusText(L"RigProfile changed to source mapping mode");
        refreshAll();
    }

    void applySelectedTemplate()
    {
        const int index = _templatePicker->getSelectedIndex();
        const auto& templates = _document->templates();
        if (index < 0 || static_cast<std::size_t>(index) >= templates.size()) {
            _host.setStatusText(L"No RigProfile template selected");
            return;
        }
        ayt::anim::editor::SkeletonTemplateApplyReport report;
        std::string error;
        if (!_document->applyTemplate(templates[index].path, &report, &error)) {
            _host.setStatusText(L"Template apply failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        std::wostringstream status;
        status << L"Template applied " << report.appliedCount
               << L" | preserved " << report.preservedCount
               << L" | missing " << report.missingCount
               << L" | ambiguous " << report.ambiguousCount;
        _host.setStatusText(status.str());
        refreshAll();
    }

    void previewSelectedTemplate()
    {
        const int index = _templatePicker->getSelectedIndex();
        const auto& templates = _document->templates();
        if (index < 0 || static_cast<std::size_t>(index) >= templates.size()) {
            _host.setStatusText(L"No RigProfile template selected");
            return;
        }
        ayt::anim::editor::SkeletonTemplateApplyReport report;
        std::string error;
        if (!_document->previewTemplate(templates[index].path, &report, &error)) {
            _host.setStatusText(L"Template preview failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        std::wostringstream preview;
        preview << L"TEMPLATE PREVIEW  |  "
                << ayt::ui::decodeUtf8Text(report.templateName)
                << L"\nWould apply: " << report.appliedCount
                << L"  |  preserve manual: " << report.preservedCount
                << L"  |  missing: " << report.missingCount
                << L"  |  ambiguous/conflict: " << report.ambiguousCount
                << L"\nPreview does not modify the mapping.";
        _diagnostics->setReport(preview.str());
        _host.setStatusText(L"RigProfile template preview ready");
    }

    void refreshHierarchy()
    {
        std::vector<std::wstring> items;
        std::vector<int> visibleBoneIds;
        const std::wstring filter = _boneSearch != nullptr
            ? lowerText(_boneSearch->getText()) : std::wstring{};
        std::vector<std::vector<std::string>> rolesByBone(
            _document->core().bones().size());
        for (const auto& spec : ayt::anim::getHumanoidBoneSpecs()) {
            const int mapped = _document->core().mapping()
                .getSourceBoneIndex(spec.role);
            if (mapped >= 0
                && static_cast<std::size_t>(mapped) < rolesByBone.size()) {
                rolesByBone[static_cast<std::size_t>(mapped)].push_back(
                    std::string(spec.canonicalName));
            }
        }
        for (const auto& bone : _document->core().bones()) {
            const std::wstring searchable = lowerText(
                ayt::ui::decodeUtf8Text(bone.name) + L" "
                + std::to_wstring(bone.index));
            if (!filter.empty() && searchable.find(filter) == std::wstring::npos) {
                continue;
            }
            std::wstring label(static_cast<std::size_t>(bone.depth * 2), L' ');
            label += ayt::ui::decodeUtf8Text(bone.name);
            label += L"  [" + std::to_wstring(bone.index) + L"]";
            const auto& mappedRoles = rolesByBone[static_cast<std::size_t>(bone.index)];
            if (!mappedRoles.empty()) {
                label += L"  → ";
                for (std::size_t roleIndex = 0; roleIndex < mappedRoles.size();
                     ++roleIndex) {
                    if (roleIndex != 0u) label += L", ";
                    label += ayt::ui::decodeUtf8Text(mappedRoles[roleIndex]);
                }
                if (mappedRoles.size() > 1u) label += L"  [DUPLICATE]";
            }
            items.push_back(std::move(label));
            visibleBoneIds.push_back(bone.index);
        }
        _syncing = true;
        _boneList->cancelPendingDrag();
        if (!_visibleBoneRows.replace(std::move(visibleBoneIds))) { _syncing = false; return; }
        _boneList->setItems(items);
        _boneList->setSelectedIndex(_visibleBoneRows.indexOf(_document->core().selectedBone()));
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
        std::vector<int> ownerCount(_document->core().bones().size(), 0);
        for (const auto& spec : ayt::anim::getHumanoidBoneSpecs()) {
            const int mapped = _document->core().mapping()
                .getSourceBoneIndex(spec.role);
            if (mapped >= 0 && static_cast<std::size_t>(mapped) < ownerCount.size()) {
                ++ownerCount[static_cast<std::size_t>(mapped)];
            }
        }
        for (const auto& spec : ayt::anim::getHumanoidBoneSpecs()) {
            const std::string roleName(spec.canonicalName);
            const bool left = roleName.starts_with("left");
            const bool right = roleName.starts_with("right");
            std::wstring label = left ? L"L " : right ? L"R " : L"· ";
            label += spec.requirement == HumanoidBoneRequirement::Required
                ? L"REQ " : L"    ";
            label += ayt::ui::decodeUtf8Text(std::string(spec.canonicalName));
            const int bone = _document->core().mapping().getSourceBoneIndex(spec.role);
            if (bone >= 0 && bone < static_cast<int>(_document->core().bones().size())) {
                label += L"  ->  " + ayt::ui::decodeUtf8Text(
                    _document->core().bones()[bone].name);
                if (ownerCount[static_cast<std::size_t>(bone)] > 1) {
                    label += L"  [DUPLICATE]";
                }
            } else if (spec.requirement == HumanoidBoneRequirement::Required) {
                label += L"  [MISSING]";
            } else {
                label += L"  [optional]";
            }
            roles.push_back(std::move(label));
        }
        _syncing = true;
        _roleList->setItems(roles);
        _roleList->setSelectedIndex(
            static_cast<int>(_document->core().selectedRole()));
        _nativeButton->setText(_document->core().isNative()
            ? L"Native: On" : L"Native: Off");
        _customButton->setText(_document->core().isNotApplicable()
            ? L"Custom: On" : L"Custom: Off");
        _syncing = false;
        refreshRolePicker();
        refreshTargetRolePicker();
        refreshCorrectionEditor();
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

    void refreshTargetRolePicker()
    {
        std::vector<std::wstring> choices{L"<Target unmapped>"};
        for (const auto& bone : _document->core().targetBones()) {
            choices.push_back(ayt::ui::decodeUtf8Text(bone.name)
                + L"  [" + std::to_wstring(bone.index) + L"]");
        }
        const int mapped = _document->core().targetMapping()
            .getSourceBoneIndex(_document->core().selectedRole());
        _syncing = true;
        _targetBonePicker->setItems(choices);
        _targetBonePicker->setSelectedIndex(mapped >= 0 ? mapped + 1 : 0);
        _targetBonePicker->setEnabled(
            _document->core().profileKind()
                == ayt::anim::editor::RigProfileKind::Retarget
            && !choices.empty());
        _syncing = false;
    }

    void refreshCorrectionEditor()
    {
        const auto& correction = _document->core().retargetCorrection(
            _document->core().selectedRole());
        const int kind = _correctionKind != nullptr
            ? _correctionKind->getSelectedIndex() : 2;
        const auto& value = kind == 0 ? correction.sourceReferenceOffset
            : kind == 1 ? correction.targetReferenceOffset
            : correction.axisCorrection;
        _correctionValue->setText(quaternionText(value));
    }

    void applyCorrection()
    {
        ayt::math::FQuaternion value;
        if (!parseQuaternionText(_correctionValue->getText(), value)) {
            _host.setStatusText(L"Correction must be x, y, z, w");
            return;
        }
        auto correction = _document->core().retargetCorrection(
            _document->core().selectedRole());
        const int kind = _correctionKind->getSelectedIndex();
        if (kind == 0) correction.sourceReferenceOffset = value;
        else if (kind == 1) correction.targetReferenceOffset = value;
        else correction.axisCorrection = value;
        if (!_document->core().setRetargetCorrection(
                _document->core().selectedRole(), correction)) {
            _host.setStatusText(L"Correction quaternion was rejected");
            return;
        }
        _host.setStatusText(L"Retarget reference/axis correction updated");
        refreshCorrectionEditor();
        refreshStatus();
    }

    void refreshBoneProperties()
    {
        const int selectedBone = _document->core().selectedBone();
        const auto selectedId = selectedBone < 0 ? std::string{} : std::to_string(selectedBone);
        _selectionBridge.publish("skeleton.bone", selectedId.empty() ? std::vector<std::string>{}
            : std::vector<std::string>{selectedId}, selectedId);
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
        const std::wstring adaptation = stateLabel(L"Mapping: ",
            SkeletonEditorCore::adaptationStateName(status.adaptation));
        if (_adaptation->getText() != adaptation) {
            _adaptation->setText(adaptation);
        }
        _adaptation->setTextColor(adaptationColor(status.adaptation));
        const std::wstring bake = stateLabel(L"Bake: ",
            SkeletonEditorCore::bakeStateName(status.bake));
        if (_bake->getText() != bake) {
            _bake->setText(bake);
        }
        _bake->setTextColor(bakeColor(status.bake));
        const std::wstring legacy = _document->core().openedLegacyMapping()
            ? L"LEGACY .aysmap | Save migrates to .ayrig | " : L"";
        const std::wstring message = legacy
            + (_document->isDirty() ? L"Modified | " : L"")
            + ayt::ui::decodeUtf8Text(status.message);
        if (_status->getText() != message) {
            _status->setText(message);
        }
        _status->setTextColor(_document->isDirty()
            ? ayt::math::FVector4{0.95f, 0.72f, 0.30f, 1.0f}
            : ayt::math::FVector4{0.62f, 0.66f, 0.74f, 1.0f});
    }

    void locateDiagnostic(const std::string& target)
    {
        for (const auto& bone : _document->core().bones()) {
            if (target != "bone." + std::to_string(bone.index)) continue;
            (void)_document->core().selectBone(bone.index);
            refreshHierarchy(); refreshBoneProperties();
            if (_canvas) _canvas->markDirty(); _host.requestRepaint(); return;
        }
    }

    std::vector<ayt::ui::authoring::DiagnosticEntry> preflightEntries(
        const ayt::anim::editor::SkeletonPreflightReport& report) const
    {
        using namespace ayt::ui::authoring;
        std::vector<DiagnosticEntry> entries;
        for (const auto& issue : report.issues) {
            const auto severity = issue.severity == ayt::anim::editor::SkeletonPreflightSeverity::Error
                ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
            const auto target = issue.boneIndex >= 0 && static_cast<std::size_t>(issue.boneIndex) < _document->core().bones().size()
                ? "bone." + std::to_string(issue.boneIndex) : std::string{};
            entries.push_back({severity, ayt::ui::decodeUtf8Text(SkeletonEditorCore::preflightCodeName(issue.code)),
                ayt::ui::decodeUtf8Text(issue.message), target});
        }
        return entries;
    }

    bool refreshPreflight()
    {
        if (!_diagnostics) return false;
        const auto report = _document->core().preflight();
        std::wostringstream heading;
        heading << (report.canBake() ? L"PREFLIGHT PASSED" : L"PREFLIGHT BLOCKED") << L"  |  "
            << report.errorCount() << L" error(s), " << report.warningCount() << L" warning(s)";
        _diagnostics->setEntries(preflightEntries(report), heading.str());
        return report.canBake();
    }

    void runDryRun()
    {
        const auto plan = _document->core().dryRunBake();
        const std::string manifestPath = SkeletonEditorCore::defaultDryRunManifestPath(
            _document->core().mappingPath());
        std::string error;
        if (!_document->core().writeDryRunManifest(plan, manifestPath, &error)) {
            _host.setStatusText(L"Bake dry run failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }

        std::wostringstream text;
        text << (plan.canBake() ? L"DRY RUN READY" : L"DRY RUN BLOCKED")
             << L"  |  keep "
             << plan.boneActionCount(
                    ayt::anim::editor::SkeletonBakeBoneAction::Keep)
             << L", rename "
             << plan.boneActionCount(
                    ayt::anim::editor::SkeletonBakeBoneAction::Rename)
             << L", delete "
             << plan.boneActionCount(
                    ayt::anim::editor::SkeletonBakeBoneAction::Delete)
             << L", dependencies " << plan.dependencies.size();
        using namespace ayt::ui::authoring;
        auto entries = preflightEntries(plan.preflight);
        for (const auto& operation : plan.boneOperations) {
            if (operation.action == ayt::anim::editor::SkeletonBakeBoneAction::Keep) continue;
            auto message = ayt::ui::decodeUtf8Text(operation.sourceName);
            if (!operation.targetName.empty()) message += L" -> " + ayt::ui::decodeUtf8Text(operation.targetName);
            const auto target = operation.sourceBoneIndex >= 0
                && static_cast<std::size_t>(operation.sourceBoneIndex) < _document->core().bones().size()
                ? "bone." + std::to_string(operation.sourceBoneIndex) : std::string{};
            entries.push_back({DiagnosticSeverity::Info, ayt::ui::decodeUtf8Text(
                SkeletonEditorCore::bakeBoneActionName(operation.action)), std::move(message), target});
        }
        for (const auto& dependency : plan.dependencies) {
            const auto severity = dependency.impact == ayt::anim::editor::SkeletonBakeDependencyImpact::Blocked
                ? DiagnosticSeverity::Error : dependency.impact == ayt::anim::editor::SkeletonBakeDependencyImpact::RequiresVerification
                ? DiagnosticSeverity::Warning : DiagnosticSeverity::Info;
            entries.push_back({severity, ayt::ui::decodeUtf8Text(SkeletonEditorCore::bakeDependencyKindName(dependency.kind)),
                ayt::ui::decodeUtf8Text(dependency.path + " " + dependency.message), {}});
        }
        _diagnostics->setEntries(std::move(entries), text.str());
        _host.setStatusText(L"Bake dry-run manifest written: "
            + ayt::ui::decodeUtf8Text(manifestPath));
    }

    void startBake()
    {
        const auto plan = _document->core().dryRunBake();
        if (!plan.canBake()) {
            (void)refreshPreflight();
            _host.setStatusText(L"Skeleton bake blocked by preflight");
            return;
        }
        const std::filesystem::path output =
            std::filesystem::path(_document->core().skeletonPath()).parent_path()
            / "Baked";
        _bakePresentation.begin(_bakeJob.start(plan, output.string()));
        _document->core().setBakeInProgress(true);
        _host.setStatusText(L"Skeleton bake started");
        pollBake();
    }

    void pollBake()
    {
        if (_bakePresentation.generation() == 0
            || (_bakePresentation.snapshot() && _bakePresentation.snapshot()->finished())) return;
        const auto snapshot = _bakeJob.poll();
        const auto presentation = bakePresentationSnapshot(snapshot);
        const auto change = _bakePresentation.observe(presentation);
        if (!change.accepted) return;
        if (snapshot.state == ayt::anim::editor::SkeletonBakeJobState::Running) {
            if (change.changed && _diagnostics != nullptr) {
                _diagnostics->setReport(ayt::ui::authoring::formatJobReport(
                    presentation, L"BAKING  | ", true));
            }
            return;
        }
        if (!change.completed) return;
        _document->core().setBakeInProgress(false);
        const bool succeeded = snapshot.state
            == ayt::anim::editor::SkeletonBakeJobState::Succeeded;
        std::string error;
        if (snapshot.state != ayt::anim::editor::SkeletonBakeJobState::Cancelled) {
            if (!_document->core().recordBakeResult(
                    succeeded, snapshot.sourceFingerprint,
                    snapshot.profileFingerprint, &error)) {
                _host.setStatusText(L"Bake result rejected: "
                    + ayt::ui::decodeUtf8Text(error));
                refreshStatus();
                return;
            }
            if (!_document->core().saveMapping(&error)) {
                _host.setStatusText(L"Bake status save failed: "
                    + ayt::ui::decodeUtf8Text(error));
                refreshStatus();
                return;
            }
        }
        if (_diagnostics != nullptr) _diagnostics->setReport(
            ayt::ui::authoring::formatJobReport(presentation, L"BAKE "
                + ayt::ui::decodeUtf8Text(ayt::anim::editor::SkeletonBakeJob::stateName(snapshot.state)),
                false, L"  |  "));
        _host.setStatusText(succeeded
            ? L"Skeleton bake completed"
            : snapshot.state == ayt::anim::editor::SkeletonBakeJobState::Cancelled
                ? L"Skeleton bake cancelled" : L"Skeleton bake failed");
        refreshStatus();
    }

    void refreshTransport()
    {
        _transport->refresh();
    }

    ayt::ui::authoring::ResourceReferenceResult loadAnimation()
    {
        std::string error;
        if (!_document->core().attachAnimation(
                encodeUtf8(_animationPath->getText()), &error)) {
            const auto message = L"Animation load failed: " + ayt::ui::decodeUtf8Text(error);
            _host.setStatusText(message); return {false, message};
        }
        _host.setStatusText(L"Animation attached to skeleton preview");
        refreshTransport();
        if (_canvas != nullptr) _canvas->markDirty();
        return {true, L"Animation attached"};
    }

    std::shared_ptr<EditorSkeletonDocument> _document;
    IEditorHostServices& _host;
    EditorCommandButtons _commands;
    EditorAuthoringSelectionBridge _selectionBridge;
    ayt::ui::Widget* _root = nullptr;
    EditorSkeletonCanvas* _canvas = nullptr;
    ayt::ui::authoring::DragSourceList* _boneList = nullptr;
    ayt::ui::TextInput* _boneSearch = nullptr;
    ayt::ui::ListView* _roleList = nullptr;
    ayt::ui::ComboBox* _bonePicker = nullptr;
    ayt::ui::ComboBox* _targetBonePicker = nullptr;
    ayt::ui::ComboBox* _correctionKind = nullptr;
    ayt::ui::ComboBox* _profilePicker = nullptr;
    ayt::ui::ComboBox* _templatePicker = nullptr;
    ayt::ui::TextArea* _properties = nullptr;
    ayt::ui::authoring::DiagnosticsPanel* _diagnostics = nullptr;
    ayt::ui::TextInput* _animationPath = nullptr;
    ayt::ui::TextInput* _targetSkeletonPath = nullptr;
    ayt::ui::TextInput* _platform = nullptr;
    ayt::ui::TextInput* _correctionValue = nullptr;
    ayt::ui::authoring::PlaybackControls* _transport = nullptr;
    ayt::ui::TextLabel* _adaptation = nullptr;
    ayt::ui::TextLabel* _bake = nullptr;
    ayt::ui::TextLabel* _status = nullptr;
    ayt::ui::Button* _nativeButton = nullptr;
    ayt::ui::Button* _customButton = nullptr;
    ayt::ui::authoring::StableListRows<int> _visibleBoneRows;
    ayt::anim::editor::SkeletonBakeJob _bakeJob;
    ayt::ui::authoring::JobPresentation _bakePresentation;
    ayt::ui::authoring::AuthoringRefreshGate _refreshGate;
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
    descriptor.extensions = {".ayskel", ".ayrig", ".aysmap"};
    descriptor.assetTypes = {"Skeleton", "Rig Profile"};
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
