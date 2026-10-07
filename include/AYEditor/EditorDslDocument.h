#pragma once

#include "AYEditor/EditorExtension.h"
#include "AYShader/ShaderAnalysis.h"
#include "AYScript/logia/Tooling.h"
#include "AYScript/logia/Debug.h"
#include <memory>
#include <functional>
#include <optional>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor {

enum class EditorDslLanguage {
    Unknown = 0,
    Phoskia,
    Logia,
    ActorClassJson,
};

enum class EditorDslDiagnosticSeverity {
    Info = 0,
    Warning,
    Error,
};

struct EditorDslDiagnostic {
    EditorDslDiagnosticSeverity severity = EditorDslDiagnosticSeverity::Error;
    int line = 0;
    int column = 0;
    std::string message;
    std::string hint;
    std::string file;
};

struct EditorDslCompileReport {
    bool success = false;
    EditorDslLanguage language = EditorDslLanguage::Unknown;
    std::string summary;
    std::vector<EditorDslDiagnostic> diagnostics;
    // Lua or BGFX generated source bytes, not GPU binary size.
    std::size_t generatedBytes = 0;
    ayt::shader::phoskia::ShaderAnalysisReport shaderAnalysis;
};

EditorDslLanguage editorDslLanguageFromPath(
    const std::string& path) noexcept;
const char* editorDslLanguageName(EditorDslLanguage language) noexcept;

// One source-backed editor document. The document owns normalized UTF-8 text,
// tracks the last saved snapshot, preserves an existing UTF-8 BOM/CRLF policy,
// and invokes the production Phoskia or Logia compiler in memory.
//
// UI widgets are intentionally absent: AYEditorSession may present this model
// in a DockCard without coupling file/compiler tests to AYUI input dispatch.
class EditorDslDocument final : public IEditorDocument {
public:
    bool open(const std::string& absolutePath,
              const std::string& displayPath,
              std::string* error = nullptr);
    bool save(std::string* error = nullptr) override;

    void setSourceUtf8(std::string source);
    const std::string& sourceUtf8() const noexcept { return _source; }
    const std::string& absolutePath() const noexcept { return _absolutePath; }
    const std::string& displayPath() const noexcept { return _displayPath; }
    EditorDslLanguage language() const noexcept { return _language; }
    const std::string& typeId() const noexcept override { return _typeId; }
    const std::string& path() const noexcept override { return _absolutePath; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _source != _savedSource; }
    uint64_t revision() const noexcept override { return _revision; }
    bool canReload() const noexcept override { return true; }
    bool reload(std::string* error = nullptr) override;
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error = nullptr) const override;

    EditorDslCompileReport compile() const;
    /// Use explicit host/schema data when embedding the editor. Otherwise infer
    /// Actor schema from assets that reference this source; use checked semantics.
    void setLogiaHostContext(ayt::script::logia::LogiaHostContext context);
    ayt::script::logia::LogiaAnalysis analyzeLogia() const;
    /// Isolated Actor preview: default Host services return unavailable values;
    /// it executes on_start with schema defaults, never the running play World.
    bool startLogiaDebug();
    bool resumeLogiaDebug(ayt::script::LogiaDebugCommand command);
    void stopLogiaDebug();
    void toggleLogiaBreakpoint(int line);
    ayt::script::LogiaDebugSnapshot logiaDebugSnapshot() const;
    const std::vector<ayt::script::LogiaDebugBreakpoint>& logiaBreakpoints() const { return _breakpoints; }
    /// Navigate a source-backed view (one-based UTF-8 source coordinates).
    void navigateTo(int line, int column);
    void setNavigationHandler(std::function<void(int,int)> handler);

private:
    struct DebugPreview;
    std::shared_ptr<DebugPreview> _debug;
    std::vector<ayt::script::LogiaDebugBreakpoint> _breakpoints;
    enum class LineEnding {
        Lf,
        CrLf,
    };

    std::string serializeForDisk() const;
    ayt::script::logia::LogiaHostContext logiaContext() const;

    std::string _absolutePath;
    std::string _displayPath;
    std::string _title;
    std::string _source;
    std::string _savedSource;
    std::string _typeId = "ayeditor.dsl";
    uint64_t _revision = 0;
    EditorDslLanguage _language = EditorDslLanguage::Unknown;
    LineEnding _lineEnding = LineEnding::Lf;
    bool _utf8Bom = false;
    std::optional<ayt::script::logia::LogiaHostContext> _logiaContext;
    std::function<void(int,int)> _navigationHandler;
    int _navigationLine = 0, _navigationColumn = 0;
};

} // namespace ayt::editor
