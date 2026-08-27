#include "RuntimeApi.h"

#include "codegen/il2cpp-codegen.h"
#include "vm/InternalCalls.h"
#include "vm/Array.h"
#include "vm/Exception.h"
#include "vm/Class.h"
#include "vm/Reflection.h"
#include "vm/String.h"
#include "utils/StringUtils.h"
#include "metadata/Assembly.h"
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
#include "vm/AssemblyShadowPrototype.h"
#endif

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
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadAssemblyShadowPrototype(System.Byte[],System.Byte[])", (Il2CppMethodPointer)LoadAssemblyShadowPrototype);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::ActivateAssemblyShadowPrototype(System.String)", (Il2CppMethodPointer)ActivateAssemblyShadowPrototype);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetAssemblyShadowPrototypeDiagnostics()", (Il2CppMethodPointer)GetAssemblyShadowPrototypeDiagnostics);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::InspectAssemblyShadowPrototypeObject(System.Object)", (Il2CppMethodPointer)InspectAssemblyShadowPrototypeObject);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::InspectAssemblyShadowPrototypeAssembly(System.Reflection.Assembly)", (Il2CppMethodPointer)InspectAssemblyShadowPrototypeAssembly);
        il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::SetAssemblyShadowPrototypePhase(System.String)", (Il2CppMethodPointer)SetAssemblyShadowPrototypePhase);
	}

    Il2CppReflectionAssembly* RuntimeApi::LoadAssemblyShadowPrototype(Il2CppArray* dllData, Il2CppArray* pdbData)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        if (!dllData || il2cpp::vm::Array::GetByteLength(dllData) == 0)
            il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetArgumentNullException("dllData"));
        auto* assembly = metadata::Assembly::CreateShadowPrototype(
            reinterpret_cast<const byte*>(il2cpp::vm::Array::GetFirstElementAddress(dllData)),
            il2cpp::vm::Array::GetByteLength(dllData),
            pdbData && il2cpp::vm::Array::GetByteLength(pdbData) ? reinterpret_cast<const byte*>(il2cpp::vm::Array::GetFirstElementAddress(pdbData)) : nullptr,
            pdbData ? il2cpp::vm::Array::GetByteLength(pdbData) : 0);
        return il2cpp::vm::Reflection::GetAssemblyObject(assembly);
#else
        RaiseNotSupportedException("native Assembly Shadow is disabled");
        return nullptr;
#endif
    }

    bool RuntimeApi::ActivateAssemblyShadowPrototype(Il2CppString* name)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return name && il2cpp::vm::AssemblyShadowPrototype::Activate(il2cpp::utils::StringUtils::Utf16ToUtf8(name->chars, name->length).c_str());
#else
        RaiseNotSupportedException("native Assembly Shadow is disabled");
        return false;
#endif
    }

    Il2CppString* RuntimeApi::GetAssemblyShadowPrototypeDiagnostics()
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return il2cpp::vm::String::New(il2cpp::vm::AssemblyShadowPrototype::Diagnostics().c_str());
#else
        return il2cpp::vm::String::New("{\"enabled\":false,\"active\":false}");
#endif
    }

    Il2CppString* RuntimeApi::InspectAssemblyShadowPrototypeObject(Il2CppObject* object)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return il2cpp::vm::String::New(il2cpp::vm::AssemblyShadowPrototype::InspectObject(object).c_str());
#else
        RaiseNotSupportedException("native Assembly Shadow is disabled");
        return nullptr;
#endif
    }

    Il2CppString* RuntimeApi::InspectAssemblyShadowPrototypeAssembly(Il2CppReflectionAssembly* assembly)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return il2cpp::vm::String::New(il2cpp::vm::AssemblyShadowPrototype::InspectAssembly(assembly ? assembly->assembly : nullptr).c_str());
#else
        RaiseNotSupportedException("native Assembly Shadow is disabled");
        return nullptr;
#endif
    }

    void RuntimeApi::SetAssemblyShadowPrototypePhase(Il2CppString* phase)
    {
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        il2cpp::vm::AssemblyShadowPrototype::SetPhase(phase ? il2cpp::utils::StringUtils::Utf16ToUtf8(phase->chars, phase->length).c_str() : "unspecified");
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
