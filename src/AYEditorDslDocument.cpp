#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "AYEditor/EditorDslDocument.h"
#include <AYAssetFormat/AssetFormat.h>

#include "AYIO/File.h"
#include <AYEntity/ActorClassAsset.h>
#include "AYScript/logia/LogiaPipeline.h"
#include "AYScript/logia/Parser.h"
#include "AYScript/ScriptRuntimeBridge.h"
#include <AYEntity/components/ActorInstanceComponent.h>
#include "AYScript/logia/EngineFunctions.h"
#include "AYShader/Phoskia.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <utility>
#include <nlohmann/json.hpp>

namespace ayt::editor {
struct EditorDslDocument::DebugPreview {
    ayt::entity::ActorScriptComponent actor;
    ayt::script::LogiaRuntimeBridge bridge;
    ayt::script::LogiaDebugSnapshot failure;
};
bool EditorDslDocument::startLogiaDebug() {
    stopLogiaDebug(); _debug=std::make_shared<DebugPreview>();
    auto fail=[this](std::string message) { _debug->failure.status=ayt::script::LogiaDebugStatus::Error;
        _debug->failure.error=std::move(message); return false; };
    auto context=logiaContext();
    if(_language!=EditorDslLanguage::Logia || context.kind!=ayt::script::logia::LogiaHostKind::Actor)
        return fail("Actor preview requires a referencing .act asset or an explicit Actor schema");
    for(const auto& [name,type]:context.actorProperties) {
        if(type=="bool")_debug->actor.properties[name]=false;
        else if(type=="string")_debug->actor.properties[name]=std::string{};
        else _debug->actor.properties[name]=0.0;
    }
    for(const auto& [name,signature]:ayt::script::logia::engineLogiaFunctions())
        _debug->bridge.registerHostFunction(name,signature,[type=signature.result](const auto&) -> ayt::script::ScriptHostValue {
            if(type=="bool")return false; if(type=="int")return std::int64_t{0};
            if(type=="float")return 0.0; if(type=="string")return std::string{};
            if(type=="vec3")return ayt::math::FVector3{}; return std::monostate{};
        });
    std::vector<ayt::script::logia::Token> tokens; ayt::script::logia::tokenize(_source,tokens);
    ayt::script::logia::Parser parser(tokens); auto ast=parser.parse();
    if(!ast || ast->scripts.size()!=1)return fail("Preview requires one script block");
    const auto name=ast->scripts.front()->name;
    std::vector<ayt::script::logia::CompilerError> errors;
    if(!_debug->bridge.loadScript(name,_source,context,errors))
        return fail(errors.empty()?"Compilation failed":errors.front().message);
    _debug->bridge.setDebugBreakpoints(_breakpoints);
    if(!_debug->bridge.startDebugActorLifecycle(name,"on_start",_debug->actor)) {
        const auto snapshot=_debug->bridge.debugSnapshot();
        return fail(snapshot.error.empty()?"Preview requires on_start":snapshot.error);
    }
    return true;
}
bool EditorDslDocument::resumeLogiaDebug(ayt::script::LogiaDebugCommand command) {
    return _debug && _debug->bridge.resumeDebug(command);
}
void EditorDslDocument::stopLogiaDebug() { _debug.reset(); }
void EditorDslDocument::toggleLogiaBreakpoint(int line) {
    if(line<1)return;
    auto found=std::find_if(_breakpoints.begin(),_breakpoints.end(),[line](const auto& point){return point.line==line;});
    if(found==_breakpoints.end())_breakpoints.push_back({_absolutePath,line}); else _breakpoints.erase(found);
    if(_debug)_debug->bridge.setDebugBreakpoints(_breakpoints);
}
ayt::script::LogiaDebugSnapshot EditorDslDocument::logiaDebugSnapshot() const {
    if(!_debug)return {};
    if(_debug->failure.status==ayt::script::LogiaDebugStatus::Error)return _debug->failure;
    return _debug->bridge.debugSnapshot();
}

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

bool validateActorClassReferences(const std::string& absolutePath,
                                  const ayt::entity::ActorClassAsset& actor,
                                  std::string& error)
{
    if (actor.parentPath.empty() && actor.scriptPath.empty()) return true;
    const auto root = ayt::entity::assetsRootForScene(absolutePath);
    if (root.empty()) {
        error = "Actor class must be inside an Assets or Content root";
        return false;
    }
    std::string path;
    if (!actor.scriptPath.empty()) {
        if (!ayt::entity::resolveActorScriptPath(root, actor.scriptPath,
                                                 path, &error)) return false;
        if (!std::filesystem::is_regular_file(path)) {
            error = "Actor script is missing: " + actor.scriptPath;
            return false;
        }
    }
    std::unordered_set<std::string> visited;
    visited.insert(std::filesystem::weakly_canonical(absolutePath).string());
    std::string next = actor.parentPath;
    while (!next.empty()) {
        if (visited.size() >= 32) {
            error = "Actor inheritance exceeds 32 classes";
            return false;
        }
        if (!ayt::entity::resolveActorClassPath(root, next, path, &error))
            return false;
        if (!visited.insert(path).second) {
            error = "Actor inheritance cycle at " + next;
            return false;
        }
        ayt::entity::ActorClassAsset parent;
        if (!ayt::entity::loadActorClassAsset(path, parent, &error)) return false;
        if (!parent.scriptPath.empty()) {
            std::string script;
            if (!ayt::entity::resolveActorScriptPath(root, parent.scriptPath,
                                                     script, &error)) return false;
            if (!std::filesystem::is_regular_file(script)) {
                error = "Actor script is missing: " + parent.scriptPath;
                return false;
            }
        }
        next = parent.parentPath;
    }
    return true;
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
        if (ayt::asset_format::matchesPath(path, ayt::asset_format::Id::ActorClass)) return EditorDslLanguage::ActorClassJson;
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
    case EditorDslLanguage::ActorClassJson: return "Actor Class";
    case EditorDslLanguage::Unknown: break;
    }
    return "DSL";
}

bool EditorDslDocument::open(const std::string& absolutePath,
                             const std::string& displayPath,
                             std::string* error)
{
    stopLogiaDebug();
    const EditorDslLanguage language =
        editorDslLanguageFromPath(absolutePath);
    if (language == EditorDslLanguage::Unknown) {
        if (error != nullptr) {
            *error = "Only .phoskia, .logia and .act files can be opened in the code editor.";
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
    stopLogiaDebug();
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

bool EditorDslDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    const std::string serialized = serializeForDisk();
    const bool written = serialized.empty()
        ? ayt::io::File::writeAllText(path, serialized)
        : ayt::io::File::atomicWrite(path, serialized.data(), serialized.size());
    if (!written && error != nullptr) {
        *error = "Failed to write DSL recovery copy.";
    } else if (error != nullptr) {
        error->clear();
    }
    return written;
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
        if (_language == EditorDslLanguage::ActorClassJson) {
            ayt::entity::ActorClassAsset actor;
            std::string error;
            report.success = ayt::entity::parseActorClassAsset(
                _source, actor, &error);
            if (report.success) {
                report.success = validateActorClassReferences(
                    _absolutePath, actor, error);
            }
            if (!report.success) {
                report.diagnostics.push_back(EditorDslDiagnostic{
                    EditorDslDiagnosticSeverity::Error, 0, 0,
                    error.empty() ? "Invalid Actor class" : error, {}});
            }
            report.summary = report.success
                ? "Actor class data is valid."
                : "Actor class data is invalid.";
            return report;
        }
        if (_language == EditorDslLanguage::Phoskia) {
            ayt::shader::phoskia::CompileOptions options;
            options.sourcePath = _absolutePath;
            ayt::shader::phoskia::analyzePhoskia(_source,options,"windows",report.shaderAnalysis);
            for (const auto& diagnostic : report.shaderAnalysis.diagnostics) {
                report.diagnostics.push_back(EditorDslDiagnostic{
                    diagnostic.severity == ayt::shader::phoskia::DiagnosticSeverity::Error
                        ? EditorDslDiagnosticSeverity::Error
                        : diagnostic.severity == ayt::shader::phoskia::DiagnosticSeverity::Warning
                            ? EditorDslDiagnosticSeverity::Warning : EditorDslDiagnosticSeverity::Info,
                    diagnostic.location.line, diagnostic.location.column, diagnostic.message,
                    diagnostic.hint, diagnostic.location.file});
            }
            report.success = report.shaderAnalysis.success;
            report.generatedBytes = report.shaderAnalysis.generatedBytes;
            report.summary = report.success
                ? "Phoskia analyzed to BGFX source (Windows); no shaderc/device validation."
                : "Phoskia compilation failed.";
            return report;
        }

        auto compiled = ayt::script::logia::compileLogiaToLua(_source, logiaContext());
        for (const auto& diagnostic : compiled.diagnostics) {
            report.diagnostics.push_back(EditorDslDiagnostic{
                toEditorSeverity(diagnostic.severity),
                diagnostic.location.line,
                diagnostic.location.column,
                diagnostic.message,
                diagnostic.hint, diagnostic.location.file});
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

ayt::script::logia::LogiaHostContext EditorDslDocument::logiaContext() const
{
    auto context = _logiaContext.value_or(ayt::script::logia::defaultLogiaHostContext());
    if (!_logiaContext) {
        bool actorHost = false;
        ayt::entity::ActorClassAsset actor;
        std::string selectedClass;
        if (!_absolutePath.empty()) {
            const auto scriptPath = std::filesystem::weakly_canonical(
                _absolutePath);
            const auto root = ayt::entity::assetsRootForScene(_absolutePath);
            auto classPath = std::filesystem::path(_absolutePath);
            classPath.replace_extension(".act");

            std::string ignored;
            bool loaded = ayt::entity::loadActorClassAsset(classPath.string(), actor,
                                                            &ignored);
            if (!loaded) {
                classPath.replace_extension(".ayactor");
                loaded = ayt::entity::loadActorClassAsset(classPath.string(), actor,
                                                           &ignored);
            }
            if (loaded) {
                selectedClass = classPath.string();
                std::string resolvedScript;
                actorHost = ayt::entity::resolveActorScriptPath(root,
                    actor.scriptPath, resolvedScript, &ignored)
                    && resolvedScript == scriptPath.string();
            }
            if (!actorHost && !root.empty()) {
                std::error_code scanError;
                for (std::filesystem::recursive_directory_iterator it(root, scanError), end;
                     !scanError && it != end; it.increment(scanError)) {
                    if (!it->is_regular_file(scanError)
                        || !ayt::asset_format::matchesPath(it->path().string(), ayt::asset_format::Id::ActorClass)) continue;
                    if (!ayt::entity::loadActorClassAsset(it->path().string(),
                                                           actor, &ignored)) continue;
                    std::string resolvedScript;
                    actorHost = ayt::entity::resolveActorScriptPath(root,
                        actor.scriptPath, resolvedScript, &ignored)
                        && resolvedScript == scriptPath.string();
                    if (actorHost) { selectedClass = it->path().string(); break; }
                }
            }
        }
        if (actorHost) {
            context = ayt::script::logia::checkedLogiaHostContext(ayt::script::logia::actorLogiaHostContext());
            std::string ignored;
            const auto root = ayt::entity::assetsRootForScene(_absolutePath);
            (void)ayt::entity::resolveActorClassAsset(root, selectedClass, actor, &ignored);
            auto properties = nlohmann::json::parse(actor.propertiesJson);
            for (auto it = properties.begin(); it != properties.end(); ++it)
                context.actorProperties[it.key()] = it.value().is_boolean() ? "bool" : it.value().is_number() ? "float" : "string";
        }
    }
    // Existing native component templates keep their legacy contract. Actor
    // documents and explicitly selected contexts already carry their mode;
    // checked syntax opts a new component/tool document into the new contract.
    if (!_logiaContext) {
        std::vector<ayt::script::logia::Token> tokens;
        ayt::script::logia::tokenize(_source, tokens);
        using Token = ayt::script::logia::TokenType;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto kind = tokens[i].type;
            if (kind == Token::State || kind == Token::Await || kind == Token::Import
                || kind == Token::OnReload
                || (kind == Token::Identifier && tokens[i].lexeme == "array"
                    && i + 1 < tokens.size() && tokens[i + 1].type == Token::Less)
                || (kind == Token::RightParen && i + 1 < tokens.size()
                    && tokens[i + 1].type == Token::Colon)) {
                context.strictSemantics = true;
                break;
            }
        }
    }
    if (!_logiaContext && context.kind == ayt::script::logia::LogiaHostKind::Component) {
        std::vector<ayt::script::logia::Token> tokens; ayt::script::logia::tokenize(_source,tokens);
        ayt::script::logia::Parser parser(tokens); auto ast=parser.parse();
        if(ast)for(const auto& script:ast->scripts)for(const auto& item:script->members)
            if(auto* fn=dynamic_cast<ayt::script::logia::LifecycleFuncDecl*>(item.get());fn&&fn->kind==ayt::script::logia::LifecycleKind::Run)
                { const bool checked = context.strictSemantics;
                  context = ayt::script::logia::toolLogiaHostContext();
                  context.strictSemantics = checked; }
    }
    context.sourcePath = _absolutePath;
    if (!_logiaContext) {
        context = ayt::script::logia::engineLogiaHostContext(std::move(context));
        std::vector<ayt::script::logia::Token> tokens;
        ayt::script::logia::tokenize(_source,tokens);
        for(std::size_t i=0;i+2<tokens.size();++i)
            if(tokens[i+1].type==ayt::script::logia::TokenType::Dot
                && context.hostFunctions.contains(tokens[i].lexeme+"."+tokens[i+2].lexeme))
                context.strictSemantics = true;
    }
    if (context.assetRoot.empty()) context.assetRoot = ayt::entity::assetsRootForScene(_absolutePath);
    return context;
}
void EditorDslDocument::setLogiaHostContext(ayt::script::logia::LogiaHostContext context) { stopLogiaDebug(); _logiaContext = std::move(context); }
ayt::script::logia::LogiaAnalysis EditorDslDocument::analyzeLogia() const
{
    if (_language != EditorDslLanguage::Logia) return {};
    return ayt::script::logia::analyzeLogia(_source, logiaContext());
}
void EditorDslDocument::navigateTo(int line, int column)
{
    _navigationLine = (std::max)(1,line); _navigationColumn = (std::max)(1,column);
    if (_navigationHandler) _navigationHandler(_navigationLine,_navigationColumn);
}
void EditorDslDocument::setNavigationHandler(std::function<void(int,int)> handler)
{
    _navigationHandler = std::move(handler);
    if (_navigationHandler && _navigationLine > 0) _navigationHandler(_navigationLine,_navigationColumn);
}
} // namespace ayt::editor
