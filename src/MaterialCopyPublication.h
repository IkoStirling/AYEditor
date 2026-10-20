#pragma once
#include <AYIO/File.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace ayt::editor::detail {
// Build a complete sibling-volume file outside the asset root, then publish
// with an exclusive hard-link operation. Existing files can never be replaced,
// including a destination created after the caller's preflight. No partial
// target is visible. Filesystems without hard-link support fail explicitly.
inline void publishNewMaterialCopy(const std::filesystem::path& sourceRoot,
                                  const std::filesystem::path& destination,
                                  const void* bytes,std::size_t size)
{
    namespace fs=std::filesystem;
    static std::atomic<unsigned long long> serial{0};
    const auto workParent=fs::canonical(sourceRoot).parent_path();
    fs::path stage;
    for(int attempt=0;attempt<8;++attempt){
        auto candidate=workParent/(".aymaterial-copy-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
        if(fs::create_directory(candidate)){stage=std::move(candidate);break;}
    }
    if(stage.empty())throw std::runtime_error("Cannot reserve material copy staging");
    struct Cleanup {fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}} cleanup{stage};
    const auto payload=stage/"material.tmp";const auto native=payload.generic_u8string();
    const std::string path(reinterpret_cast<const char*>(native.data()),native.size());
    if(!io::File::atomicWriteUtf8(path,bytes,size))throw std::runtime_error("Cannot write complete material copy");
    fs::create_directories(destination.parent_path());
    std::error_code error;fs::create_hard_link(payload,destination,error);
    if(error)throw std::runtime_error("Cannot publish a new material without overwriting an existing file: "+error.message());
}
} // namespace ayt::editor::detail
