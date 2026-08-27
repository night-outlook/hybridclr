#pragma once

#include <stdint.h>
#include "CommonDef.h"

namespace hybridclr
{
    class AssemblyShadowRuntimeApi
    {
    public:
        static void RegisterInternalCalls();

        static int32_t ConfigureCandidates(Il2CppString* baselineBuildId,
            Il2CppArray* candidateAssemblyNames, Il2CppArray* stableAotNames);
        static int32_t BeginTransaction(Il2CppString* patchId,
            Il2CppString* expectedBaselineBuildId, Il2CppArray* closureLoadOrder,
            int32_t runtimeAbiVersion);
        static int32_t StageAssembly(Il2CppArray* dllBytes, Il2CppArray* pdbBytes);
        static int32_t ValidateTransaction();
        static int32_t CommitTransaction();
        static int32_t AbortTransaction();
        static int32_t GetState(int32_t* state);
        static int32_t GetAssemblyExecutionMode(Il2CppString* logicalAssemblyName, int32_t* mode);
        static int32_t GetDiagnosticsJson(Il2CppString** json);
    };
}
