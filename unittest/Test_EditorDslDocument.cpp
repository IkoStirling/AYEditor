#include "AYTest.h"

#include "AYEditor/EditorDslDocument.h"
#include "AYEditor/EditorDslExtension.h"
#include "AYEditor/EditorSession.h"
#include "AYEditor/EditorWorkspace.h"
#include "AYApplication.h"
#include "AYApplication/IEngineHost.h"
#include "AYIO/File.h"
#include "AYUI/DockArea.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/TextArea.h"
#include "AYUI/Widget.h"
#include "AYUI/TileView.h"

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace ayt::editor;

namespace {

class DslTestHostServices final : public IEditorHostServices {
public:
    explicit DslTestHostServices(EditorWorkspace& workspace)
        : _workspace(workspace) {}

    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override {
        return _projectRoot;
    }
    void requestRepaint() override { ++repaintRequests; }
    void setStatusText(const std::wstring& text) override {
        statusText = text;
    }

    int repaintRequests = 0;
    std::wstring statusText;

private:
    EditorWorkspace& _workspace;
    std::string _projectRoot;
};

std::filesystem::path dslEditorTempRoot(const char* suffix)
{
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / (std::string("ayeditor_dsl_") + suffix + "_"
           + std::to_string(nonce));
}

struct DslEditorTempCleanup {
    std::filesystem::path root;
    ~DslEditorTempCleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::string dslEditorLayoutPath()
{
    const std::filesystem::path path =
        AY_EDITOR_TEST_SOURCE_DIR "/ui/editor_shell.ui.json";
    return std::filesystem::exists(path) ? path.string() : std::string{};
}

ayt::math::FVector2 dslRectCenter(
    const ayt::math::FRectangle& rectangle)
{
    return {
        (rectangle.minX + rectangle.maxX) * 0.5f,
        (rectangle.minY + rectangle.maxY) * 0.5f};
}

std::vector<ayt::ui::TextArea*> dslTextAreas(ayt::ui::Widget* root)
{
    std::vector<ayt::ui::TextArea*> result;
    if (root == nullptr) return result;
    std::vector<ayt::ui::Widget*> pending{root};
    while (!pending.empty()) {
        ayt::ui::Widget* widget = pending.back();
        pending.pop_back();
        if (auto* area = dynamic_cast<ayt::ui::TextArea*>(widget)) {
            result.push_back(area);
        }
        for (ayt::ui::Widget* child : widget->getChildren()) {
            if (child != nullptr) pending.push_back(child);
        }
    }
    return result;
}

} // namespace

TEST_SUITE(AYEditor_DslDocument)

TEST_CASE(dsl_language_detection_and_asset_classification_are_case_insensitive)
{
    CHECK(editorDslLanguageFromPath("Material.PHOSKIA")
          == EditorDslLanguage::Phoskia);
    CHECK(editorDslLanguageFromPath("Player.Logia")
          == EditorDslLanguage::Logia);
    CHECK(editorDslLanguageFromPath("notes.txt")
          == EditorDslLanguage::Unknown);
    CHECK(classifyEditorAssetPath("Material.phoskia")
          == EditorAssetType::Shader);
    CHECK(classifyEditorAssetPath("Player.logia")
          == EditorAssetType::Script);
}

TEST_CASE(dsl_extension_opens_real_document_and_builds_workspace_view)
{
    DslEditorTempCleanup cleanup{dslEditorTempRoot("workspace")};
    const std::filesystem::path file = cleanup.root / "Assets/Tool.logia";
    CHECK(ayt::io::File::createParentDirectories(file.string()));
    CHECK(ayt::io::File::writeAllText(
        file.string(), "script Tool {\n}\n"));

    EditorWorkspace workspace;
    std::string error;
    CHECK(registerEditorDslExtension(workspace.registry(), &error));
    CHECK(error.empty());

    EditorOpenRequest request;
    request.resourcePath = file.string();
    request.resourceKey = file.string();
    request.displayPath = "Assets/Tool.logia";
    const EditorOpenResult first = workspace.documents().open(request);
    const EditorOpenResult duplicate = workspace.documents().open(request);
    CHECK(first.status == EditorOpenStatus::Opened);
    CHECK(duplicate.status == EditorOpenStatus::FocusedExisting);
    CHECK(first.documentId == duplicate.documentId);
    CHECK(workspace.documents().size() == 1u);

    auto document = std::dynamic_pointer_cast<EditorDslDocument>(
        first.document);
    CHECK(document != nullptr);
    CHECK(document != nullptr && document->displayPath()
          == "Assets/Tool.logia");
    const uint64_t revision = document != nullptr
        ? document->revision() : 0u;
    if (document != nullptr) {
        document->setSourceUtf8(
            "script Tool {\n    on_start() { }\n}\n");
    }
    CHECK(document != nullptr && document->revision() == revision + 1u);
    CHECK(document != nullptr && document->isDirty());

    const EditorDescriptor* descriptor = workspace.registry().find(
        kEditorDslExtensionId);
    CHECK(descriptor != nullptr);
    DslTestHostServices hostServices(workspace);
    std::unique_ptr<IEditorView> view = descriptor != nullptr
        ? descriptor->createView(first.document, hostServices) : nullptr;
    CHECK(view != nullptr);
    CHECK(view != nullptr && view->rootWidget() != nullptr);
    CHECK(view != nullptr && view->commandTarget() != nullptr);
    ayt::ui::Widget* root = view != nullptr
        ? view->releaseRootWidget() : nullptr;
    CHECK(root != nullptr);
    if (root != nullptr) ayt::ui::destroyWidgetTree(root);
    view.reset();

    CHECK(workspace.documents().close(first.documentId,
        EditorDocumentCloseAction::Save));
    CHECK(workspace.documents().size() == 0u);
    CHECK(ayt::io::File::readAllText(file.string()).find("on_start")
          != std::string::npos);
}

TEST_CASE(dsl_document_save_preserves_utf8_bom_and_crlf)
{
    DslEditorTempCleanup cleanup{dslEditorTempRoot("encoding")};
    const std::filesystem::path file = cleanup.root / "Assets/Test.logia";
    CHECK(ayt::io::File::createParentDirectories(file.string()));
    const std::string original =
        std::string("\xef\xbb\xbf", 3) + "script Test {\r\n}\r\n";
    CHECK(ayt::io::File::writeAllText(file.string(), original));

    EditorDslDocument document;
    std::string error;
    CHECK(document.open(file.string(), "Assets/Test.logia", &error));
    CHECK(error.empty());
    CHECK(document.sourceUtf8() == "script Test {\n}\n");
    CHECK_FALSE(document.isDirty());

    document.setSourceUtf8("script Test {\n    on_start() { }\n}\n");
    CHECK(document.isDirty());
    CHECK(document.save(&error));
    CHECK_FALSE(document.isDirty());
    const std::vector<std::uint8_t> bytes =
        ayt::io::File::readAllBytes(file.string());
    CHECK(bytes.size() >= 3u);
    CHECK(bytes.size() >= 3u && bytes[0] == 0xefu);
    CHECK(bytes.size() >= 3u && bytes[1] == 0xbbu);
    CHECK(bytes.size() >= 3u && bytes[2] == 0xbfu);
    const std::string saved(bytes.begin() + 3, bytes.end());
    CHECK(saved.find("\r\n") != std::string::npos);
    bool hasOnlyCrLfLineEndings = true;
    for (std::size_t index = 0; index < saved.size(); ++index) {
        if (saved[index] == '\n'
            && (index == 0 || saved[index - 1] != '\r')) {
            hasOnlyCrLfLineEndings = false;
            break;
        }
    }
    CHECK(hasOnlyCrLfLineEndings);
}

TEST_CASE(dsl_document_invokes_production_logia_and_phoskia_compilers)
{
    DslEditorTempCleanup cleanup{dslEditorTempRoot("compile")};
    const std::filesystem::path logia = cleanup.root / "Empty.logia";
    const std::filesystem::path phoskia = cleanup.root / "Unlit.phoskia";
    CHECK(ayt::io::File::createParentDirectories(logia.string()));
    CHECK(ayt::io::File::writeAllText(logia.string(), "script Empty {\n}\n"));
    CHECK(ayt::io::File::writeAllText(phoskia.string(), R"(
material Unlit {
    vertex { return vec4(0.0, 0.0, 0.0, 1.0) }
    fragment { return vec4(1.0, 1.0, 1.0, 1.0) }
}
)"));

    EditorDslDocument logiaDocument;
    EditorDslDocument phoskiaDocument;
    CHECK(logiaDocument.open(logia.string(), "Empty.logia"));
    CHECK(phoskiaDocument.open(phoskia.string(), "Unlit.phoskia"));
    const EditorDslCompileReport logiaOk = logiaDocument.compile();
    const EditorDslCompileReport phoskiaOk = phoskiaDocument.compile();
    CHECK(logiaOk.success);
    CHECK(logiaOk.generatedBytes > 0u);
    CHECK(phoskiaOk.success);

    logiaDocument.setSourceUtf8("script Broken { on_start() {");
    phoskiaDocument.setSourceUtf8(
        "material Broken { vertex { return vec4(0.0) ");
    const EditorDslCompileReport logiaFailed = logiaDocument.compile();
    const EditorDslCompileReport phoskiaFailed = phoskiaDocument.compile();
    CHECK_FALSE(logiaFailed.success);
    CHECK_FALSE(phoskiaFailed.success);
    CHECK_FALSE(logiaFailed.diagnostics.empty());
    CHECK_FALSE(phoskiaFailed.diagnostics.empty());
}

TEST_CASE(engine_player_controller_template_compiles)
{
#ifndef AY_EDITOR_TEST_SOURCE_DIR
#error "AY_EDITOR_TEST_SOURCE_DIR must point at EngineAssets/AYEditor"
#endif
    const std::filesystem::path templatePath =
        std::filesystem::path(AY_EDITOR_TEST_SOURCE_DIR).parent_path()
        / "AYScript" / "templates" / "player_controller.logia";
    CHECK(std::filesystem::is_regular_file(templatePath));
    if (!std::filesystem::is_regular_file(templatePath)) return;

    EditorDslDocument document;
    std::string error;
    CHECK(document.open(templatePath.string(),
                        "EngineAssets/AYScript/templates/player_controller.logia",
                        &error));
    CHECK(error.empty());
    if (!error.empty()) return;
    const EditorDslCompileReport report = document.compile();
    CHECK(report.success);
    CHECK(report.generatedBytes > 0u);
}

TEST_CASE(content_browser_double_click_opens_one_dsl_tab_and_shortcuts_work)
{
    const std::string layout = dslEditorLayoutPath();
    CHECK(!layout.empty());
    if (layout.empty()) return;

    DslEditorTempCleanup cleanup{dslEditorTempRoot("session")};
    const std::filesystem::path file = cleanup.root / "Assets/Tool.logia";
    CHECK(ayt::io::File::createParentDirectories(file.string()));
    CHECK(ayt::io::File::writeAllText(file.string(), "script Tool {\n}\n"));

    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer renderer;
    EditorSessionDesc desc;
    desc.uiBackend = &renderer;
    desc.layoutPath = layout;
    desc.projectRoot = cleanup.root.string();
    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280.0f, 720.0f);
    CHECK(session.rescanAssetsNow());

    auto* list = dynamic_cast<ayt::ui::TileView*>(
        session.ui().findById("list_assets"));
    auto* dock = dynamic_cast<ayt::ui::DockArea*>(
        session.ui().findById("main_dock"));
    CHECK(list != nullptr);
    CHECK(dock != nullptr);
    CHECK(list != nullptr && list->getItemCount() == 1u);
    if (list == nullptr || dock == nullptr || list->getItemCount() != 1u) {
        session.shutdown();
        return;
    }

    ayt::ui::TileCell* cell = list->cellForLogicalIndex(0);
    CHECK(cell != nullptr);
    if (cell == nullptr) {
        session.shutdown();
        return;
    }
    const ayt::math::FVector2 click = session.ui().logicalToPhysical(
        dslRectCenter(cell->getThumbnailRect()));
    for (int count = 0; count < 2; ++count) {
        session.onMouseMove(click.x, click.y);
        (void)session.onMouseButtonDown(click.x, click.y, 0);
        (void)session.onMouseButtonUp(click.x, click.y, 0);
        if (count == 0) session.update(0.1f);
    }
    session.update(0.0f);
    CHECK(session.openDslDocumentCount() == 1u);
    CHECK(session.workspace().documents().size() == 1u);
    CHECK(session.workspace().documents().active() != nullptr);
    CHECK(session.workspace().documents().active() != nullptr
          && session.workspace().documents().active()->editorId
             == kEditorDslExtensionId);

    const EditorAssetRecord& record = session.assetDatabase().records().front();
    const std::string cardId = "card_dsl_" + std::to_string(record.id);
    ayt::ui::DockCard* card = dock->findCard(cardId);
    CHECK(card != nullptr);
    if (card == nullptr) {
        session.shutdown();
        return;
    }
    std::vector<ayt::ui::TextArea*> areas = dslTextAreas(card);
    CHECK(areas.size() == 2u);
    if (areas.size() != 2u) {
        session.shutdown();
        return;
    }
    // DFS returns diagnostics before source for this VBox child order.
    ayt::ui::TextArea* diagnostics = areas[0];
    ayt::ui::TextArea* source = areas[1];
    CHECK(source->getText().find(L"script Tool") != std::wstring::npos);
    CHECK(source->areLineNumbersVisible());
    CHECK(source->doesTabInsertIndent());
    CHECK(source->getTabWidth() == 4u);
    CHECK(source->hasSyntaxHighlighter());

    // The session must apply the code-editor presentation, not merely expose
    // dormant TextArea capabilities. Render the source in a deterministic
    // local rectangle and witness both the gutter and the Logia keyword span.
    renderer.clear();
    source->setSize(ayt::math::FVector2(420.0f, 96.0f));
    source->performLayout();
    source->render(renderer);
    bool drewLineNumber = false;
    bool coloredScriptKeyword = false;
    for (const ayt::ui::MockRenderer::DrawCall& call :
         renderer.getDrawCalls()) {
        if (call.type != ayt::ui::MockRenderer::DrawCall::Text) continue;
        if (call.text == L"1") drewLineNumber = true;
        if (call.text == L"script"
            && std::abs(call.color.x - 0.78f) < 0.02f
            && std::abs(call.color.y - 0.52f) < 0.02f) {
            coloredScriptKeyword = true;
        }
    }
    CHECK(drewLineNumber);
    CHECK(coloredScriptKeyword);

    // A DSL tab occupies the same Center geometry as Scene View. Its text
    // document must win hit-testing instead of the stale viewport rectangle
    // starting a scene-pick gesture. The compact unit-test layout leaves the
    // fill source pane at zero height, so use the fixed diagnostics pane for
    // this routing assertion; source editing is exercised below via focus.
    session.render(true);
    const ayt::math::FVector2 diagnosticsClick =
        session.ui().logicalToPhysical(dslRectCenter(
            diagnostics->getDocumentAsFocusable()->getWorldBounds()));
    CHECK(session.onMouseButtonDown(
        diagnosticsClick.x, diagnosticsClick.y, 0));
    CHECK(session.ui().getFocusedWidget()
          == diagnostics->getDocumentAsFocusable());
    (void)session.onMouseButtonUp(
        diagnosticsClick.x, diagnosticsClick.y, 0);

    bool diagnosticsWerePainted = false;
    bool diagnosticsBoundsAreValid = false;
    for (const ayt::ui::MockRenderer::DrawCall& call :
         renderer.getDrawCalls()) {
        if (call.type != ayt::ui::MockRenderer::DrawCall::Text) continue;
        if (call.text.find(L"No compilation run yet")
            == std::wstring::npos) {
            continue;
        }
        diagnosticsWerePainted = true;
        diagnosticsBoundsAreValid = call.bounds.maxX > call.bounds.minX
            && call.bounds.maxY > call.bounds.minY;
    }
    CHECK(diagnosticsWerePainted);
    CHECK(diagnosticsBoundsAreValid);

    source->setText(L"script Tool {\n}\n");
    source->setCaret(1, 0);
    session.ui().setFocus(source->getDocumentAsFocusable());
    CHECK(session.onKeyDown(ayt::ui::UIKey_Tab));
    CHECK(source->getText() == L"script Tool {\n    }\n");

    source->setText(L"script Tool {\n    on_start() { }\n}\n");
    CHECK(card->getTitle().find(L"*") != std::wstring::npos);
    session.ui().setFocus(source->getDocumentAsFocusable());
    (void)session.onKeyDown(ayt::ui::UIKey_Control);
    CHECK(session.onKeyDown(ayt::ui::UIKey_S));
    (void)session.onKeyUp(ayt::ui::UIKey_S);
    (void)session.onKeyUp(ayt::ui::UIKey_Control);
    CHECK(card->getTitle().find(L"*") == std::wstring::npos);
    CHECK(ayt::io::File::readAllText(file.string()).find("on_start")
          != std::string::npos);

    CHECK(session.onKeyDown(ayt::ui::UIKey_F7));
    CHECK(diagnostics->getText().find(L"[Success]") != std::wstring::npos);
    source->setText(L"script Broken { on_start() {");
    CHECK(session.onKeyDown(ayt::ui::UIKey_F7));
    CHECK(diagnostics->getText().find(L"[Failed]") != std::wstring::npos);

    CHECK(session.openDslAsset(record.id));
    CHECK(session.openDslDocumentCount() == 1u);
    CHECK(session.workspace().documents().size() == 1u);
    session.shutdown();
    CHECK(session.workspace().documents().size() == 0u);
}

TEST_SUITE_END
