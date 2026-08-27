#include "RuntimeApi.h"
#include "AssemblyShadowRuntimeApi.h"

#include "codegen/il2cpp-codegen.h"
#include "vm/InternalCalls.h"
#include "vm/Array.h"
#include "vm/Exception.h"
#include "vm/Class.h"
#include "vm/Reflection.h"
#include "vm/String.h"
#include "utils/StringUtils.h"
#include "metadata/Assembly.h"
#include "vm/AssemblyShadow.h"

#include "metadata/MetadataModule.h"
#include "metadata/MetadataUtil.h"
#include "interpreter/InterpreterModule.h"
#include "RuntimeConfig.h"

namespace hybridclr
{
	void RuntimeApi::RegisterInternalCalls()
	{
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadMetadataForAOTAssembly(System.Byte[],HybridCLR.HomologousImageMode)", (Il2CppMethodPointer)LoadMetadataForAOTAssembly);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetRuntimeOption(HybridCLR.RuntimeOptionId)", (Il2CppMethodPointer)GetRuntimeOption);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::SetRuntimeOption(HybridCLR.RuntimeOptionId,System.Int32)", (Il2CppMethodPointer)SetRuntimeOption);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PreJitClass(System.Type)", (Il2CppMethodPointer)PreJitClass);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PreJitMethod(System.Reflection.MethodInfo)", (Il2CppMethodPointer)PreJitMethod);
        AssemblyShadowRuntimeApi::RegisterInternalCalls();
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadAssemblyShadowPrototype(System.Byte[],System.Byte[])", (Il2CppMethodPointer)LoadAssemblyShadowPrototype);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::ActivateAssemblyShadowPrototype(System.String)", (Il2CppMethodPointer)ActivateAssemblyShadowPrototype);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetAssemblyShadowPrototypeDiagnostics()", (Il2CppMethodPointer)GetAssemblyShadowPrototypeDiagnostics);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::InspectAssemblyShadowPrototypeObject(System.Object)", (Il2CppMethodPointer)InspectAssemblyShadowPrototypeObject);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::InspectAssemblyShadowPrototypeAssembly(System.Reflection.Assembly)", (Il2CppMethodPointer)InspectAssemblyShadowPrototypeAssembly);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::SetAssemblyShadowPrototypePhase(System.String)", (Il2CppMethodPointer)SetAssemblyShadowPrototypePhase);
	}

    Il2CppReflectionAssembly* RuntimeApi::LoadAssemblyShadowPrototype(Il2CppArray* dllData, Il2CppArray* pdbData)
    {
        (void)dllData;
        (void)pdbData;
        RaiseNotSupportedException("Assembly Shadow Prototype was retired; use the M03 transaction API");
        return nullptr;
    }

    bool RuntimeApi::ActivateAssemblyShadowPrototype(Il2CppString* name)
    {
        (void)name;
        RaiseNotSupportedException("Assembly Shadow Prototype was retired; use the M03 transaction API");
        return false;
    }

    Il2CppString* RuntimeApi::GetAssemblyShadowPrototypeDiagnostics()
    {
        std::string json;
        il2cpp::vm::AssemblyShadowError result = il2cpp::vm::AssemblyShadow::GetDiagnosticsJson(json);
        if (result == il2cpp::vm::AssemblyShadowError::FeatureDisabled || json.empty())
            return il2cpp::vm::String::New("{\"enabled\":false}");
        return il2cpp::vm::String::New(json.c_str());
    }

    Il2CppString* RuntimeApi::InspectAssemblyShadowPrototypeObject(Il2CppObject* object)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return il2cpp::vm::String::New(il2cpp::vm::AssemblyShadow::InspectObject(object).c_str());
#else
        RaiseNotSupportedException("Assembly Shadow physical inspection requires native IL2CPP");
        return nullptr;
#endif
    }

    Il2CppString* RuntimeApi::InspectAssemblyShadowPrototypeAssembly(Il2CppReflectionAssembly* assembly)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return il2cpp::vm::String::New(il2cpp::vm::AssemblyShadow::InspectAssembly(assembly ? assembly->assembly : nullptr).c_str());
#else
        RaiseNotSupportedException("Assembly Shadow physical inspection requires native IL2CPP");
        return nullptr;
#endif
    }

    void RuntimeApi::SetAssemblyShadowPrototypePhase(Il2CppString* phase)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        il2cpp::vm::AssemblyShadow::SetPhase(phase ? il2cpp::utils::StringUtils::Utf16ToUtf8(phase->chars, phase->length).c_str() : "unspecified");
#else
        (void)phase;
#endif
    }

	int32_t RuntimeApi::LoadMetadataForAOTAssembly(Il2CppArray* dllBytes, int32_t mode)
	{
		if (!dllBytes)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		return (int32_t)hybridclr::metadata::Assembly::LoadMetadataForAOTAssembly(il2cpp::vm::Array::GetFirstElementAddress(dllBytes), il2cpp::vm::Array::GetByteLength(dllBytes), (hybridclr::metadata::HomologousImageMode)mode);
	}

	int32_t RuntimeApi::GetRuntimeOption(int32_t optionId)
	{
		return hybridclr::RuntimeConfig::GetRuntimeOption((hybridclr::RuntimeOptionId)optionId);
	}

	void RuntimeApi::SetRuntimeOption(int32_t optionId, int32_t value)
	{
		hybridclr::RuntimeConfig::SetRuntimeOption((hybridclr::RuntimeOptionId)optionId, value);
	}

	int32_t PreJitMethod0(const MethodInfo* methodInfo);

	int32_t RuntimeApi::PreJitClass(Il2CppReflectionType* type)
	{
		if (metadata::HasNotInstantiatedGenericType(type->type))
		{
			return false;
		}
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type->type, false);
		if (!klass)
		{
			return false;
		}
		metadata::Image* image = metadata::MetadataModule::GetImage(klass->image);
		if (!image)
		{
			image = (metadata::Image*)hybridclr::metadata::AOTHomologousImage::FindImageByAssembly(
				klass->rank ? il2cpp_defaults.corlib->assembly : klass->image->assembly);
			if (!image)
			{
				return false;
			}
		}
		for (uint16_t i = 0; i < klass->method_count; i++)
		{
			const MethodInfo* methodInfo = klass->methods[i];
			PreJitMethod0(methodInfo);
		}
		return true;
	}

	int32_t PreJitMethod0(const MethodInfo* methodInfo)
	{
		if (!methodInfo->isInterpterImpl)
		{
			return false;
		}
		if (methodInfo->klass->is_generic)
		{
			return false;
		}
		if (!methodInfo->is_inflated)
		{
			if (methodInfo->is_generic)
			{
				return false;
			}
		}
		else
		{
			const Il2CppGenericMethod* genericMethod = methodInfo->genericMethod;
			if (metadata::HasNotInstantiatedGenericType(genericMethod->context.class_inst) || metadata::HasNotInstantiatedGenericType(genericMethod->context.method_inst))
			{
				return false;
			}
		}

		return interpreter::InterpreterModule::GetInterpMethodInfo(methodInfo) != nullptr;
	}

	int32_t RuntimeApi::PreJitMethod(Il2CppReflectionMethod* method)
	{
		return PreJitMethod0(method->method);
	}
}
