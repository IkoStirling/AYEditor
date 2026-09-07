#include "AYEditor/EditorDslDocument.h"

#include "AYIO/File.h"
#include "AYScript/logia/LogiaPipeline.h"
#include "AYShader/Phoskia.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>

namespace ayt::editor {

namespace {

constexpr std::size_t kMaximumDslSourceBytes = 8u * 1024u * 1024u;

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string normalizeLineEndings(const std::string& source,
                                 bool& usedCrLf)
{
    usedCrLf = source.find("\r\n") != std::string::npos;
    std::string normalized;
    normalized.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        if (ch != '\r') {
            normalized.push_back(ch);
            continue;
        }
        if (i + 1 < source.size() && source[i + 1] == '\n') {
            ++i;
        }
        normalized.push_back('\n');
    }
    return normalized;
}

EditorDslDiagnosticSeverity toEditorSeverity(
    ayt::script::logia::DiagnosticSeverity severity)
{
    using Source = ayt::script::logia::DiagnosticSeverity;
    switch (severity) {
    case Source::Info: return EditorDslDiagnosticSeverity::Info;
    case Source::Warning: return EditorDslDiagnosticSeverity::Warning;
    case Source::Error: break;
    }
    return EditorDslDiagnosticSeverity::Error;
}

void appendUnexpectedFailure(EditorDslCompileReport& report,
                             const std::string& message)
{
    report.success = false;
    report.summary = "Compiler invocation failed.";
    report.diagnostics.push_back(EditorDslDiagnostic{
        EditorDslDiagnosticSeverity::Error, 0, 0, message, {}});
}

} // namespace

EditorDslLanguage editorDslLanguageFromPath(
    const std::string& path) noexcept
{
    try {
        const std::string extension = lowerAscii(
            std::filesystem::path(path).extension().string());
        if (extension == ".phoskia") return EditorDslLanguage::Phoskia;
        if (extension == ".logia") return EditorDslLanguage::Logia;
    } catch (...) {
        // Invalid platform path syntax is simply not an editor DSL.
    }
    return EditorDslLanguage::Unknown;
}

const char* editorDslLanguageName(EditorDslLanguage language) noexcept
{
    switch (language) {
    case EditorDslLanguage::Phoskia: return "Phoskia";
    case EditorDslLanguage::Logia: return "Logia";
    case EditorDslLanguage::Unknown: break;
    }
    return "DSL";
}

bool EditorDslDocument::open(const std::string& absolutePath,
                             const std::string& displayPath,
                             std::string* error)
{
    const EditorDslLanguage language =
        editorDslLanguageFromPath(absolutePath);
    if (language == EditorDslLanguage::Unknown) {
        if (error != nullptr) {
            *error = "Only .phoskia and .logia files can be opened in the DSL editor.";
        }
        return false;
    }
    if (!ayt::io::File::exists(absolutePath)) {
        if (error != nullptr) *error = "DSL source file does not exist.";
        return false;
    }

    const ayt::io::FileAttributes attributes =
        ayt::io::File::queryAttributes(absolutePath);
    if (attributes.size > kMaximumDslSourceBytes) {
        if (error != nullptr) {
            *error = "DSL source is larger than the 8 MiB editor limit.";
        }
        return false;
    }

    std::string diskSource = ayt::io::File::readAllText(absolutePath);
    if (diskSource.empty() && attributes.size != 0) {
        if (error != nullptr) *error = "DSL source could not be read.";
        return false;
    }

    bool utf8Bom = false;
    if (diskSource.size() >= 3
        && static_cast<unsigned char>(diskSource[0]) == 0xefu
        && static_cast<unsigned char>(diskSource[1]) == 0xbbu
        && static_cast<unsigned char>(diskSource[2]) == 0xbfu) {
        utf8Bom = true;
        diskSource.erase(0, 3);
    }
    bool usedCrLf = false;
    std::string normalized = normalizeLineEndings(diskSource, usedCrLf);

    _absolutePath = absolutePath;
    _displayPath = displayPath.empty() ? absolutePath : displayPath;
    try {
        _title = std::filesystem::path(_displayPath).filename().string();
    } catch (...) {
        _title = _displayPath;
    }
    if (_title.empty()) _title = _displayPath;
    _language = language;
    _lineEnding = usedCrLf ? LineEnding::CrLf : LineEnding::Lf;
    _utf8Bom = utf8Bom;
    _source = std::move(normalized);
    _savedSource = _source;
    ++_revision;
    if (error != nullptr) error->clear();
    return true;
}

void EditorDslDocument::setSourceUtf8(std::string source)
{
    bool ignoredCrLf = false;
    std::string normalized = normalizeLineEndings(source, ignoredCrLf);
    if (_source == normalized) return;
    _source = std::move(normalized);
    ++_revision;
}

std::string EditorDslDocument::serializeForDisk() const
{
    std::string serialized;
    const std::size_t extra = _lineEnding == LineEnding::CrLf
        ? static_cast<std::size_t>(std::count(
              _source.begin(), _source.end(), '\n'))
        : 0u;
    serialized.reserve(_source.size() + extra + (_utf8Bom ? 3u : 0u));
    if (_utf8Bom) serialized.append("\xef\xbb\xbf", 3);
    if (_lineEnding == LineEnding::Lf) {
        serialized += _source;
        return serialized;
    }
    for (const char ch : _source) {
        if (ch == '\n') serialized.push_back('\r');
        serialized.push_back(ch);
    }
    return serialized;
}

bool EditorDslDocument::save(std::string* error)
{
    if (_absolutePath.empty() || _language == EditorDslLanguage::Unknown) {
        if (error != nullptr) *error = "No DSL source is open.";
        return false;
    }
    const std::string serialized = serializeForDisk();
    const bool written = serialized.empty()
        ? ayt::io::File::writeAllText(_absolutePath, serialized)
        : ayt::io::File::atomicWrite(
              _absolutePath, serialized.data(), serialized.size());
    if (!written) {
        if (error != nullptr) *error = "Failed to write the DSL source file.";
        return false;
    }
    _savedSource = _source;
    if (error != nullptr) error->clear();
    return true;
}

bool EditorDslDocument::reload(std::string* error)
{
    if (_absolutePath.empty()) {
        if (error != nullptr) *error = "No DSL source is open.";
        return false;
    }
    const std::string absolutePath = _absolutePath;
    const std::string displayPath = _displayPath;
    return open(absolutePath, displayPath, error);
}

EditorDslCompileReport EditorDslDocument::compile() const
{
    EditorDslCompileReport report;
    report.language = _language;
    if (_language == EditorDslLanguage::Unknown) {
        appendUnexpectedFailure(report, "No supported DSL language is selected.");
        return report;
    }

    try {
        if (_language == EditorDslLanguage::Phoskia) {
            auto compiler =
                std::make_unique<ayt::shader::phoskia::Compiler>();
            ayt::shader::phoskia::CompileResult compiled;
            compiler->compile(_source, compiled);
            for (const auto& error : compiled.errors) {
                report.diagnostics.push_back(EditorDslDiagnostic{
                    EditorDslDiagnosticSeverity::Error,
                    error.line, error.column, error.message, {}});
            }
            for (const std::string& warning : compiled.warnings) {
                report.diagnostics.push_back(EditorDslDiagnostic{
                    EditorDslDiagnosticSeverity::Warning,
                    0, 0, warning, {}});
            }
            report.success = compiled.success && compiled.errors.empty();
            report.summary = report.success
                ? "Phoskia compiled to the BGFX shader-source backend."
                : "Phoskia compilation failed.";
            return report;
        }

        ayt::script::logia::LogiaToLuaResult compiled =
            ayt::script::logia::compileLogiaToLua(_source);
        for (const auto& diagnostic : compiled.diagnostics) {
            report.diagnostics.push_back(EditorDslDiagnostic{
                toEditorSeverity(diagnostic.severity),
                diagnostic.location.line,
                diagnostic.location.column,
                diagnostic.message,
                diagnostic.hint});
        }
        // Older parser/codegen failures may only populate the legacy error
        // channel. Avoid duplicating the full diagnostic channel when present.
        if (report.diagnostics.empty()) {
            for (const auto& error : compiled.errors) {
                report.diagnostics.push_back(EditorDslDiagnostic{
                    EditorDslDiagnosticSeverity::Error,
                    error.line, error.column, error.message, {}});
            }
        }
        report.success = compiled.success;
        report.generatedBytes = compiled.lua.size();
        report.summary = report.success
            ? "Logia compiled to Lua successfully."
            : "Logia compilation failed.";
    } catch (const std::exception& exception) {
        appendUnexpectedFailure(report, exception.what());
    } catch (...) {
        appendUnexpectedFailure(report, "Unknown compiler exception.");
    }
    return report;
}

} // namespace ayt::editor
