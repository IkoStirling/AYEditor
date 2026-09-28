#include "AYTest.h"
#include "AYTestJson.h"
#include "AYTestProcess.h"
#include "AYGameLoop.h"
#include <AYEntity/EntityModule.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <set>

namespace {
std::string utf8Path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}
int runIsolatedEditor(const std::filesystem::path& executable, const ayt::test::RunOptions& options) {
    using namespace ayt::test;
    const auto selected = selectTests(options);
    if (selected.empty() || options.list) return runTests("AYEditor", options);
    const auto finalPath = options.report_json.empty() ? std::filesystem::path{} :
        std::filesystem::absolute(options.report_json);
    const auto reportRoot = testTmpDir() / "reports";
    std::filesystem::create_directories(reportRoot);
    TestReport combined;
    combined.module = "AYEditor isolated suites";
    combined.completed = true;
    std::set<std::string> suites;
    for (const auto* test : selected) suites.insert(test->suite);
    const double started = getTimeMs();
    for (const auto& suite : suites) {
        const auto childPath = reportRoot / (suite + ".json");
        std::vector<std::string> args{"--aytest-child", "--suite", suite, "--report-json", utf8Path(childPath)};
        if (!options.case_name.empty()) { args.push_back("--case"); args.push_back(options.case_name); }
        for (const auto& excluded : options.exclude_cases) { args.push_back("--exclude-case"); args.push_back(excluded); }
        if (options.verbose) args.push_back("--verbose");
        printf("\n[ISOLATED SUITE] %s\n", suite.c_str());
        fflush(stdout);
        const auto process = runIsolatedProcess(executable, args, std::chrono::seconds(120));
        TestReport child;
        child.module = suite;
        std::string error;
        try {
            std::ifstream file(childPath);
            if (!readJsonReport(nlohmann::json::parse(file), child, error)) child.errors.push_back(error);
        } catch (const std::exception& e) { child.errors.push_back(std::string("Missing or invalid child report: ") + e.what()); }
        std::set<std::pair<std::string, std::string>> expected, actual;
        for (const auto* test : selected) if (suite == test->suite) expected.emplace(test->suite, test->name);
        for (const auto& result : child.results) actual.emplace(result.suite, result.name);
        if (child.has_active_case) actual.emplace(child.active_case.suite, child.active_case.name);
        if (child.selected_cases != static_cast<int>(expected.size()) ||
            (child.completed && actual != expected) ||
            !std::includes(expected.begin(), expected.end(), actual.begin(), actual.end())) {
            child.completed = false;
            child.errors.push_back("Child selection differs from registered inventory: " + suite);
        }
        // Retain the parent's known selected count even if startup produced no report.
        child.selected_cases = static_cast<int>(expected.size());
        resolveProcessReport(child, process);
        if (!appendReport(combined, child, error)) {
            combined.completed = false;
            combined.errors.push_back(error);
        }
    }
    combined.elapsed_ms = getTimeMs() - started;
    if (!finalPath.empty()) {
        std::string error;
        if (!writeJsonReport(finalPath, combined, error)) {
            combined.completed = false;
            combined.errors.push_back(error);
        }
    }
    printReport(combined);
    printf("Child report directory: %s\n", utf8Path(reportRoot).c_str());
    return !combined.completed || !combined.errors.empty() || summarizeReport(combined).failed_cases ? 1 : 0;
}
}
int main(int argc, char* argv[]) {
    using namespace ayt::test;
    bool child = argc > 1 && std::string(argv[1]) == "--aytest-child";
    std::vector<char*> arguments{argv[0]};
    for (int i = child ? 2 : 1; i < argc; ++i) arguments.push_back(argv[i]);
    RunOptions options;
    bool help = false;
    if (parseRunOptions(static_cast<int>(arguments.size()), arguments.data(), options, help)) return 2;
    if (help || options.list) return runTests("AYEditor", static_cast<int>(arguments.size()), arguments.data());
    if (!child) return runIsolatedEditor(std::filesystem::absolute(argv[0]), options);
    ayt::entity::registerEntityComponents();
    const int result = runTests("AYEditor", options);
    ayt::game::GameLoop::instance().shutdown();
    return result;
}
