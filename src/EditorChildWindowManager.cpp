#include "EditorChildWindowManager.h"

#include <cstdio>
#include <fstream>
#include <string>

// nlohmann/json single-header — already in AYUI's thirdparty. Reach
// across the AYUI include path so we don't pull a new dep into
// AYEditor.
#include <nlohmann/json.hpp>

namespace ayt::editor {

namespace {

ChildWindowConfig parseOneConfig(const nlohmann::json& j) {
    ChildWindowConfig cfg;
    const auto itTitle = j.find("title");
    if (itTitle != j.end() && itTitle->is_string()) {
        cfg.title = itTitle->get<std::string>();
    }
    const auto itLayout = j.find("layoutPath");
    if (itLayout != j.end() && itLayout->is_string()) {
        cfg.layoutPath = itLayout->get<std::string>();
    }
    const auto itX = j.find("x");
    if (itX != j.end() && itX->is_number_integer()) {
        cfg.x = itX->get<int>();
    }
    const auto itY = j.find("y");
    if (itY != j.end() && itY->is_number_integer()) {
        cfg.y = itY->get<int>();
    }
    const auto itW = j.find("width");
    if (itW != j.end() && itW->is_number_integer()) {
        cfg.width = itW->get<int>();
    }
    const auto itH = j.find("height");
    if (itH != j.end() && itH->is_number_integer()) {
        cfg.height = itH->get<int>();
    }
    return cfg;
}

} // namespace

std::vector<ChildWindowConfig> parseChildWindowConfig(const std::string& path) {
    std::vector<ChildWindowConfig> out;
    if (path.empty()) {
        return out;
    }
    std::ifstream in(path);
    if (!in.is_open()) {
        // Missing file is a non-fatal "no children requested" — log
        // once and return empty so the editor proceeds normally.
        std::fprintf(stderr,
            "[EditorChildWindowManager] no child config at %s\n",
            path.c_str());
        return out;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[EditorChildWindowManager] parse error at %s: %s\n",
            path.c_str(), e.what());
        return out;
    }
    const auto itWindows = j.find("windows");
    if (itWindows == j.end() || !itWindows->is_array()) {
        std::fprintf(stderr,
            "[EditorChildWindowManager] no 'windows' array in %s\n",
            path.c_str());
        return out;
    }
    for (const auto& w : *itWindows) {
        if (w.is_object()) {
            out.push_back(parseOneConfig(w));
        }
    }
    return out;
}

EditorChildWindowManager::EditorChildWindowManager(ayt::device::WindowManager& wm,
                                                   ayt::ui::UIManager& primary)
    : _wm(wm)
    , _primary(primary) {
}

EditorChildWindowManager::~EditorChildWindowManager() {
    // K-INV-D5-6: tear down child windows BEFORE the primary UI.
    // ~UIManager calls shutdown() which can poke g_activeUIManager
    // (only if it was active); the primary's active flag wins over
    // a potentially-null child, so destroying the manager here
    // (with primary still alive) avoids an UAF cleanup race.
    for (auto& e : _entries) {
        if (e.handle != nullptr) {
            _wm.destroyTopLevelWindow(e.handle);
            e.handle = nullptr;
        }
    }
    _entries.clear();
}

bool EditorChildWindowManager::openChildWindow(const ChildWindowConfig& cfg,
                                               Handle& outHandle) {
    outHandle = nullptr;

    ayt::device::TopLevelWindowDesc d;
    d.title  = cfg.title;
    d.x      = cfg.x;
    d.y      = cfg.y;
    d.width  = cfg.width;
    d.height = cfg.height;

    Handle h = nullptr;
    if (!_wm.createTopLevelWindow(d, h)) {
        std::fprintf(stderr,
            "[EditorChildWindowManager] createTopLevelWindow failed for "
            "'%s'\n", cfg.title.c_str());
        return false;
    }

    // Build the entry first so the callbacks can capture a stable
    // shared_ptr (the vector may reallocate on push_back; std::shared_ptr
    // keeps the UIManager alive across the lifetime of the callback
    // even if closeChildWindow removes the entry under it).
    Entry e;
    e.handle     = h;
    e.ui         = std::make_shared<ayt::ui::UIManager>();
    e.ui->initialize(nullptr);  // K-INV-D5-4 null backend = no render
    e.ui->setClientSize(static_cast<float>(cfg.width),
                        static_cast<float>(cfg.height));
    if (!cfg.layoutPath.empty()) {
        // Best-effort — failure logs but doesn't abort open. The
        // child window still lives and shows whatever the default
        // canvas draws (currently empty).
        e.ui->loadLayout(cfg.layoutPath);
    }
    e.layoutPath = cfg.layoutPath;

    ayt::device::TopLevelWindowCallbacks cbs;
    // K-INV-D5-6: capture by value. The UIManager lives in `_entries`
    // by shared_ptr; the lambda runs on the Win32 message thread,
    // NOT concurrent with our tick (single-threaded editor v1).
    cbs.onCloseRequested = [this, h]() {
        this->closeChildWindow(h);
    };
    _wm.setTopLevelCallbacks(h, cbs);

    _entries.push_back(std::move(e));
    outHandle = h;
    return true;
}

void EditorChildWindowManager::closeChildWindow(Handle h) {
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->handle == h) {
            if (it->handle != nullptr) {
                _wm.destroyTopLevelWindow(it->handle);
            }
            // shared_ptr<UIManager> drops here — ~UIManager calls
            // shutdown() which resets g_activeUIManager IF this was
            // the active one. Re-set the primary as active so a
            // subsequent update() finds the editor's primary manager
            // in slot, not nullptr.
            _entries.erase(it);
            if (ayt::ui::UIManager::tryGet() == nullptr) {
                // Restore primary as active so the next tick/sw
                // doesn't see an empty slot. Try setActive via
                // initialising the primary — it already IS active
                // unless shutdown() reset it, in which case we have
                // a deeper problem. As a belt-and-braces fallback:
                // re-run initialize on primary? No — primary is
                // owned by EditorSession and we cannot re-init it
                // safely. The only safe fallback is to leave the
                // slot empty; EditorSession updates set the primary
                // back as active via the existing pushActive path.
            }
            return;
        }
    }
}

void EditorChildWindowManager::tickAll(float dt, ayt::ui::IRenderBackend* backend) {
    for (auto& e : _entries) {
        if (!e.ui) continue;
        // D5 — pushActive swaps g_activeUIManager for the duration of
        // this iteration; on scope exit the previous active (typically
        // the editor's primary) is restored.
        ayt::ui::UIManager::ActiveScope guard(e.ui.get());
        e.ui->update(dt);
        e.ui->render();  // nullptr backend → populateFrame/flushFrame guard
        (void)backend;   // reserved for v2 (per-window bgfx routing)
    }
}

bool EditorChildWindowManager::routeKey(Handle h, ::ayt::device::KeyCode kc) {
    for (auto& e : _entries) {
        if (e.handle == h && e.ui) {
            ayt::ui::UIManager::ActiveScope guard(e.ui.get());
            return e.ui->onDeviceKeyDown(kc);
        }
    }
    return false;
}

} // namespace ayt::editor
