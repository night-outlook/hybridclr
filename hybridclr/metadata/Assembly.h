#pragma once

#include "../CommonDef.h"

#include "InterpreterImage.h"
#include "AOTHomologousImage.h"
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include "vm/AssemblyShadowTypes.h"
#include <string>
#endif

namespace hybridclr
{
namespace metadata
{
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
    struct StagedAssembly;
#endif

    class Assembly
    {
    public:
        static void InitializePlaceHolderAssemblies();
        static Il2CppAssembly* LoadFromBytes(const void* assemblyData, uint64_t length, const void* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        static il2cpp::vm::AssemblyShadowError ReadStagedAssemblyIdentity(const byte* dll, size_t dllLength, std::string& name, std::string& detail);
        static il2cpp::vm::AssemblyShadowError CreateStagedSkeleton(const byte* dll, size_t dllLength, const byte* pdb, size_t pdbLength, StagedAssembly*& staged, std::string& detail);
        static il2cpp::vm::AssemblyShadowError InitializeStagedRuntimeMetadata(StagedAssembly* staged, std::string& detail);
        static void PublishStagedImage(StagedAssembly* staged);
        static il2cpp::vm::AssemblyShadowError RunStagedModuleInitializer(StagedAssembly* staged, std::string& detail);
#endif
        static LoadImageErrorCode LoadMetadataForAOTAssembly(const void* dllBytes, uint32_t dllSize, HomologousImageMode mode);
    private:
        static Il2CppAssembly* Create(const byte* assemblyData, uint64_t length, const byte* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength);
    };
}
}
