#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor {
// CPU-owned, top-left, straight-alpha RGBA8. File paths are UTF-8.
struct EditorRasterImage {
    int width=0,height=0;
    std::vector<uint8_t> rgba;
    explicit operator bool() const noexcept {return width>0 && height>0 && !rgba.empty();}
};
// Inspect dimensions before allocating decoded pixels; no GPU/cache dependency.
EditorRasterImage loadEditorRasterImage(const std::string& path,std::string* error=nullptr,
    uint64_t pixelBudget=64u*1024u*1024u);
}
