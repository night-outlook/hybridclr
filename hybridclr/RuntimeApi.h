#pragma once

#include <stdint.h>
#include "CommonDef.h"

namespace hybridclr
{
	class RuntimeApi
	{
	public:
		static void RegisterInternalCalls();

		static int32_t LoadMetadataForAOTAssembly(Il2CppArray* dllData, int32_t mode);

        // M01 source-compatibility symbols. Loading/activation are retired;
        // diagnostics/inspection forward to the generalized Assembly Shadow API.
        static Il2CppReflectionAssembly* LoadAssemblyShadowPrototype(Il2CppArray* dllData, Il2CppArray* pdbData);
        static bool ActivateAssemblyShadowPrototype(Il2CppString* name);
        static Il2CppString* GetAssemblyShadowPrototypeDiagnostics();
        static Il2CppString* InspectAssemblyShadowPrototypeObject(Il2CppObject* object);
        static Il2CppString* InspectAssemblyShadowPrototypeAssembly(Il2CppReflectionAssembly* assembly);
        static void SetAssemblyShadowPrototypePhase(Il2CppString* phase);

		static int32_t GetRuntimeOption(int32_t optionId);
		static void SetRuntimeOption(int32_t optionId, int32_t value);

		static int32_t PreJitClass(Il2CppReflectionType* type);
		static int32_t PreJitMethod(Il2CppReflectionMethod* method);
	};
}
