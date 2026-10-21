#include "EditorDebugVisualization.h"
#include "EditorDebugPrimitives.h"
#include <AYRenderer.h>
#include <AYRenderer/ResourceResidency.h>
#include <sstream>

namespace ayt::editor {
namespace {
const char* filterName(ayt::render::ObservedTextureFilter filter){
    using F=ayt::render::ObservedTextureFilter;
    switch(filter){case F::Point:return "point";case F::Linear:return "linear";
        case F::AnisotropicDeviceMaximum:return "anisotropic (device maximum)";default:return "unknown";}
}
const char* addressName(ayt::render::ObservedTextureAddress value){
    using A=ayt::render::ObservedTextureAddress;
    switch(value){case A::Repeat:return "repeat";case A::Mirror:return "mirror";
        case A::Clamp:return "clamp";case A::Border:return "border";default:return "unknown";}
}
}
std::vector<ayt::physics::PhysicsDebugRecord> editorRendererResourceDebugRecords(
    const ayt::render::Renderer& renderer,uint32_t categories)
{
    using namespace debug_detail;
    if(!enabled(categories,Category::Resource))return {};
    Collector out;out.categories=categories;
    const auto snapshot=renderer.resourceResidencySnapshot(24);
    Record summary;summary.category=Category::Resource;summary.label="Renderer texture residency";
    if(!snapshot.available){summary.details="Renderer resource observation is unavailable before initialization.";
        out.append(std::move(summary));return out.finish();}
    std::ostringstream text;
    text<<"Resource context "<<snapshot.contextId<<" | table entries: meshes "<<snapshot.meshEntries
        <<", materials "<<snapshot.materialEntries<<", textures "<<snapshot.textureEntries
        <<"\nOwned/borrowed textures "<<snapshot.ownedTextures<<" / "<<snapshot.borrowedTextures
        <<"\nKnown owned logical texture bytes "<<snapshot.knownOwnedLogicalTextureBytes
        <<" | unknown owned storage entries "<<snapshot.unknownOwnedStorageTextures
        <<"\nCurrent creation payload bytes "<<snapshot.knownOwnedCreationUploadBytes
        <<" | unknown payload entries "<<snapshot.unknownOwnedUploadTextures
        <<"\nRetained CPU image bytes "<<snapshot.retainedCpuImageBytes
        <<" | omitted texture rows "<<snapshot.textureRowsOmitted
        <<"\nLogical texture estimates exclude borrowed storage, pass/target-pool/device helpers, retired allocations and driver overhead."
        <<"\nCreation payload is not cumulative bandwidth. Table counts are not Scene reference counts.";
    summary.details=text.str();out.append(std::move(summary));
    for(const auto& texture:snapshot.textures){
        Record record;record.category=Category::Resource;record.queryId=uint32_t(texture.textureId);
        record.label="Renderer texture #"+std::to_string(texture.textureId);
        if(!texture.cacheKeys.empty())record.label+=" | "+texture.cacheKeys.front();
        std::ostringstream details;
        details<<texture.width<<"x"<<texture.height<<" | "<<(texture.borrowed?"borrowed":texture.dynamic?"dynamic":"owned")
            <<" | format "<<(texture.storageDescriptionKnown?texture.storageFormat:"unknown")
            <<" | levels "<<(texture.storageDescriptionKnown?std::to_string(texture.mipCount):"unknown")
            <<" | transfer "<<(texture.srgbKnown?(texture.srgb?"sRGB":"linear"):"unknown")
            <<"\nLogical storage bytes "<<(texture.logicalStorageBytesKnown?std::to_string(texture.logicalStorageBytes):"unknown")
            <<" | creation payload bytes "<<(texture.creationUploadBytesKnown?std::to_string(texture.creationUploadBytes):"unknown")
            <<" | retained CPU image bytes "<<texture.retainedCpuImageBytes;
        if(texture.sourcePayloadKnown)details<<"\nProvided pixel/block payload "<<texture.sourceFormat<<" | "
            <<texture.sourceMipCount<<" levels | "<<texture.sourcePayloadBytes<<" bytes (not file/package size)";
        details<<"\nCreation min/mag/mip: "<<filterName(texture.minFilter)<<" / "<<filterName(texture.magFilter)
            <<" / "<<filterName(texture.mipFilter)<<" | address U/V/W "<<addressName(texture.addressU)
            <<" / "<<addressName(texture.addressV)<<" / "<<addressName(texture.addressW)
            <<"\nPartial-chain fallback "<<texture.partialChainFallback<<" | sampler fallback "<<texture.samplerFallback
            <<"\nCache keys "<<texture.cacheKeyCount<<" | keys truncated "<<texture.cacheKeysTruncated
            <<"\nOpaque ids and copied rows do not retain GPU resources; draw-specific sampler overrides are not observed.";
        record.details=details.str();out.append(std::move(record));
    }
    return out.finish();
}
} // namespace ayt::editor
