#include "AYEditor/EditorDslExtension.h"

#include "AYEditor/EditorDslDocument.h"
#include "AYUI/Box.h"
#include "AYUI/Button.h"
#include "AYUI/TextArea.h"
#include "AYUI/TextLabel.h"
#include "AYUI/UnicodeText.h"
#include "AYUI/Widget.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace ayt::editor {
namespace {

void appendUtf8(std::string& output, uint32_t codePoint)
{
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

std::string encodeUtf8(const std::wstring& text)
{
    std::string output;
    output.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        uint32_t codePoint = static_cast<uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu
                && index + 1 < text.size()) {
                const uint32_t low = static_cast<uint32_t>(text[index + 1]);
                if (low >= 0xdc00u && low <= 0xdfffu) {
                    codePoint = 0x10000u
                        + ((codePoint - 0xd800u) << 10u)
                        + (low - 0xdc00u);
                    ++index;
                }
            }
        }
        if (codePoint >= 0xd800u && codePoint <= 0xdfffu) {
            codePoint = 0xfffdu;
        }
        appendUtf8(output, std::min(codePoint, uint32_t{0x10ffffu}));
    }
    return output;
}

const char* severityName(EditorDslDiagnosticSeverity severity) noexcept
{
    switch (severity) {
    case EditorDslDiagnosticSeverity::Info: return "info";
    case EditorDslDiagnosticSeverity::Warning: return "warning";
    case EditorDslDiagnosticSeverity::Error: break;
    }
    return "error";
}

std::wstring formatCompileReport(const EditorDslCompileReport& report,
                                 const std::string& displayPath)
{
    std::ostringstream output;
    output << (report.success ? "[Success] " : "[Failed] ")
           << displayPath << '\n' << report.summary;
    if (report.generatedBytes != 0) {
        output << " Generated output: " << report.generatedBytes << " bytes.";
    }
    if (report.diagnostics.empty()) {
        output << "\nNo diagnostics.";
    } else {
        for (const EditorDslDiagnostic& diagnostic : report.diagnostics) {
            output << "\n\n" << severityName(diagnostic.severity);
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

bool isIdentifierStart(wchar_t ch) noexcept
{
    return ch == L'_' || std::iswalpha(static_cast<wint_t>(ch)) != 0;
}

bool isIdentifierContinue(wchar_t ch) noexcept
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
    size_t index = 0;
    while (index < line.size()) {
        if (line[index] == L'/' && index + 1 < line.size()
            && line[index + 1] == L'/') {
            spans.push_back(Span{
                index, line.size() - index, commentColor});
            break;
        }
        if (line[index] == L'"' || line[index] == L'\'') {
            const size_t start = index;
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
            const size_t start = index++;
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
        if (isIdentifierStart(line[index])) {
            const size_t start = index++;
            while (index < line.size() && isIdentifierContinue(line[index])) {
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

class EditorDslWorkspaceView final
    : public IEditorView, public IEditorCommandTarget {
public:
    EditorDslWorkspaceView(std::shared_ptr<EditorDslDocument> document,
                           IEditorHostServices& host)
        : _document(std::move(document)), _host(host)
    {
        buildWidgetTree();
    }

    ~EditorDslWorkspaceView() override
    {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override
    {
        ayt::ui::Widget* root = _root;
        _root = nullptr;
        return root;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }

    bool handlesCommand(const std::string& commandId) const override
    {
        return commandId == "file.save"
            || commandId == kEditorDslCompileCommand
            || commandId == "edit.undo"
            || commandId == "edit.redo";
    }

    bool canExecuteCommand(const std::string& commandId) const override
    {
        if (commandId == "edit.undo") {
            return _source != nullptr && _source->canUndo();
        }
        if (commandId == "edit.redo") {
            return _source != nullptr && _source->canRedo();
        }
        return handlesCommand(commandId) && _document != nullptr;
    }

    bool executeCommand(const std::string& commandId) override
    {
        if (!canExecuteCommand(commandId)) return false;
        if (commandId == "file.save") return save();
        if (commandId == kEditorDslCompileCommand) {
            compile();
            return true;
        }
        if (commandId == "edit.undo") _source->undo();
        if (commandId == "edit.redo") _source->redo();
        syncSourceFromWidget();
        return true;
    }

private:
    void buildWidgetTree()
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(4.0f);
        root->setPadding(6.0f, 5.0f, 6.0f, 6.0f);

        auto* toolbar = new ayt::ui::HBox();
        toolbar->setSpacing(5.0f);
        auto* language = new ayt::ui::TextLabel();
        language->setText(ayt::ui::decodeUtf8Text(
            editorDslLanguageName(_document->language())));
        language->setFontSize(12);
        language->setTextColor(
            ayt::math::FVector4(0.42f, 0.72f, 1.0f, 1.0f));
        language->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);

        auto* saveButton = new ayt::ui::Button();
        saveButton->setText(L"Save");
        saveButton->setAccessibilityLabel(L"Save DSL source");
        saveButton->setPadding(7.0f, 3.0f, 7.0f, 3.0f);
        auto* compileButton = new ayt::ui::Button();
        compileButton->setText(L"Compile");
        compileButton->setAccessibilityLabel(L"Compile current DSL buffer");
        compileButton->setPadding(7.0f, 3.0f, 7.0f, 3.0f);

        _status = new ayt::ui::TextLabel();
        _status->setFontSize(12);
        _status->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        toolbar->addWidget(language, 68.0f);
        toolbar->addWidget(saveButton, 54.0f);
        toolbar->addWidget(compileButton, 68.0f);
        toolbar->addWidget(_status, 0.0f);
        root->addWidget(toolbar, 26.0f);

        _source = new ayt::ui::TextArea();
        _source->setWordWrap(false);
        _source->setLineHeight(17.0f);
        _source->setMaxLength(8u * 1024u * 1024u);
        _source->setLineNumbersVisible(true);
        _source->setTabInsertsIndent(true);
        _source->setTabWidth(4u);
        const EditorDslLanguage sourceLanguage = _document->language();
        _source->setSyntaxHighlighter(
            [sourceLanguage](size_t, const std::wstring& line) {
                return highlightDslLine(sourceLanguage, line);
            });
        _source->setText(
            ayt::ui::decodeUtf8Text(_document->sourceUtf8()));
        root->addWidget(_source, 0.0f);

        auto* diagnosticsHeader = new ayt::ui::TextLabel();
        diagnosticsHeader->setText(L"DIAGNOSTICS");
        diagnosticsHeader->setFontSize(11);
        diagnosticsHeader->setTextColor(
            ayt::math::FVector4(0.58f, 0.62f, 0.70f, 1.0f));
        diagnosticsHeader->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        root->addWidget(diagnosticsHeader, 18.0f);

        _diagnostics = new ayt::ui::TextArea();
        _diagnostics->setReadOnly(true);
        _diagnostics->setWordWrap(true);
        _diagnostics->setLineHeight(16.0f);
        _diagnostics->setText(
            L"No compilation run yet. Compile uses the current editor buffer.");
        root->addWidget(_diagnostics, 116.0f);

        _source->setOnTextChanged(
            [this](const std::wstring&) { syncSourceFromWidget(); });
        saveButton->setOnClicked([this]() { (void)save(); });
        compileButton->setOnClicked([this]() { compile(); });
        refreshStatus();
    }

    void syncSourceFromWidget()
    {
        if (_source == nullptr || _document == nullptr) return;
        _document->setSourceUtf8(encodeUtf8(_source->getText()));
        refreshStatus();
    }

    void refreshStatus()
    {
        if (_status == nullptr || _document == nullptr) return;
        _status->setText(_document->isDirty()
            ? L"Modified  |  Ctrl+S save  |  F7 compile"
            : L"Ready  |  F7 compile");
        _status->setTextColor(_document->isDirty()
            ? ayt::math::FVector4(0.95f, 0.72f, 0.30f, 1.0f)
            : ayt::math::FVector4(0.64f, 0.68f, 0.75f, 1.0f));
        _host.requestRepaint();
    }

    bool save()
    {
        std::string error;
        if (!_document->save(&error)) {
            _status->setText(
                L"Save failed: " + ayt::ui::decodeUtf8Text(error));
            _status->setTextColor(
                ayt::math::FVector4(0.95f, 0.35f, 0.35f, 1.0f));
            _diagnostics->setText(
                L"[Save failed] " + ayt::ui::decodeUtf8Text(error));
            _host.setStatusText(L"DSL save failed");
            _host.requestRepaint();
            return false;
        }
        refreshStatus();
        _status->setText(
            L"Saved " + ayt::ui::decodeUtf8Text(_document->displayPath()));
        _status->setTextColor(
            ayt::math::FVector4(0.42f, 0.78f, 0.52f, 1.0f));
        _host.setStatusText(
            L"Saved DSL: "
            + ayt::ui::decodeUtf8Text(_document->displayPath()));
        return true;
    }

    void compile()
    {
        const EditorDslCompileReport report = _document->compile();
        _diagnostics->setText(
            formatCompileReport(report, _document->displayPath()));
        _diagnostics->setCaret(0, 0);
        _status->setText(report.success
            ? (_document->isDirty()
                ? L"Compile succeeded (unsaved buffer)"
                : L"Compile succeeded")
            : L"Compile failed - see Diagnostics");
        _status->setTextColor(report.success
            ? ayt::math::FVector4(0.42f, 0.78f, 0.52f, 1.0f)
            : ayt::math::FVector4(0.95f, 0.35f, 0.35f, 1.0f));
        _host.setStatusText(
            ayt::ui::decodeUtf8Text(
                editorDslLanguageName(report.language))
            + (report.success ? L" compile succeeded" : L" compile failed"));
        _host.requestRepaint();
    }

    std::shared_ptr<EditorDslDocument> _document;
    IEditorHostServices& _host;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextArea* _source = nullptr;
    ayt::ui::TextArea* _diagnostics = nullptr;
    ayt::ui::TextLabel* _status = nullptr;
};

} // namespace

EditorDescriptor makeEditorDslDescriptor()
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorDslExtensionId;
    descriptor.displayName = L"Phoskia / Logia Editor";
    descriptor.iconPath = "icons/outline/code.svg";
    descriptor.surfaceKind = EditorSurfaceKind::Document;
    descriptor.openPolicy = EditorOpenPolicy::PerResource;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.priority = 100;
    descriptor.extensions = {".phoskia", ".logia"};
    descriptor.assetTypes = {"shader", "script"};
    descriptor.createDocument =
        [](const EditorOpenRequest& request,
           std::string& error) -> std::shared_ptr<IEditorDocument> {
            auto document = std::make_shared<EditorDslDocument>();
            const std::string displayPath = request.displayPath.empty()
                ? request.resourcePath : request.displayPath;
            if (!document->open(request.resourcePath, displayPath, &error)) {
                return nullptr;
            }
            return document;
        };
    descriptor.createView =
        [](const std::shared_ptr<IEditorDocument>& document,
           IEditorHostServices& host) -> std::unique_ptr<IEditorView> {
            auto dsl = std::dynamic_pointer_cast<EditorDslDocument>(document);
            if (dsl == nullptr) return nullptr;
            return std::make_unique<EditorDslWorkspaceView>(
                std::move(dsl), host);
        };
    return descriptor;
}

bool registerEditorDslExtension(EditorExtensionRegistry& registry,
                                std::string* error)
{
    return registry.registerEditor(makeEditorDslDescriptor(), error);
}

} // namespace ayt::editor
