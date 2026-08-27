#pragma once

#include "../CommonDef.h"

#include "InterpreterImage.h"
#include "AOTHomologousImage.h"

namespace hybridclr
{
namespace metadata
{

    class Assembly
    {
    public:
        static void InitializePlaceHolderAssemblies();
        static Il2CppAssembly* LoadFromBytes(const void* assemblyData, uint64_t length, const void* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        // M01 experiment: leaves module initialization deferred and is not unloadable.
        static Il2CppAssembly* CreateShadowPrototype(const byte* assemblyData, uint64_t length, const byte* pdbData, uint64_t pdbLength);
#endif
        static LoadImageErrorCode LoadMetadataForAOTAssembly(const void* dllBytes, uint32_t dllSize, HomologousImageMode mode);
    private:
        static Il2CppAssembly* Create(const byte* assemblyData, uint64_t length, const byte* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
            , bool shadowPrototype = false
#endif
        );
    };
}
}
