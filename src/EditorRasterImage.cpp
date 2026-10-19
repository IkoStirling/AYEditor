#include "EditorRasterImage.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>

namespace ayt::editor {
EditorRasterImage loadEditorRasterImage(const std::string& path,std::string* error,uint64_t budget) {
    if(error)error->clear();
    const auto fail=[&](const char* text){if(error)*error=text;return EditorRasterImage{};};
    if(path.empty())return fail("Choose a source image first");
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);
    if(!file)return fail("The source image could not be opened");
    const auto length=file.tellg();
    if(length<=0 || uint64_t(length)>256u*1024u*1024u)return fail("The encoded image exceeds the 256 MiB authoring budget");
    std::vector<uint8_t> bytes(size_t(length),uint8_t{});file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
    if(!file)return fail("The source image could not be read completely");
    int width=0,height=0,components=0;
    if(!stbi_info_from_memory(bytes.data(),int(bytes.size()),&width,&height,&components) || width<=0 || height<=0)
        return fail("The source is not a supported raster image");
    const uint64_t count=uint64_t(width)*uint64_t(height);
    if(count>budget || width>std::numeric_limits<uint16_t>::max() || height>std::numeric_limits<uint16_t>::max())
        return fail("The source image exceeds the authoring pixel budget");
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(),int(bytes.size()),&width,&height,&components,4),&stbi_image_free);
    if(!pixels)return fail("The source image could not be decoded");
    EditorRasterImage result;result.width=width;result.height=height;
    result.rgba.assign(pixels.get(),pixels.get()+size_t(count)*4);return result;
}
}
