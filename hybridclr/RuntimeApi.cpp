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

#if HYBRIDCLR_H1_COUNT_DIAGNOSTICS
#include "metadata/InterpreterImage.h"
#include "metadata/RawImageBase.h"
#include "metadata/MetadataUtil.h"
#include "vm/Assembly.h"
#include "vm/AssemblyName.h"
#include "vm/MetadataLock.h"
#include "vm/AssemblyShadowDiagnostics.h"
#include <exception>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>
#endif

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
#if HYBRIDCLR_H1_COUNT_DIAGNOSTICS
        il2cpp::vm::InternalCalls::Add("AssemblyShadowDemo.H1CountNativeDiagnostics::GetSnapshot(System.String&)", (Il2CppMethodPointer)GetH1CountDiagnosticsJson);
        il2cpp::vm::InternalCalls::Add("AssemblyShadowDemo.H1CountEarlyStartup::GetSnapshot(System.String&)", (Il2CppMethodPointer)GetH1CountDiagnosticsJson);
#endif
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

#if HYBRIDCLR_H1_COUNT_DIAGNOSTICS
    namespace
    {
        using AssemblyVector = il2cpp::vm::AssemblyVector;

        static std::string NativePointer(const void* value)
        {
            if (!value)
                return std::string();
            std::ostringstream out;
            out << "0x" << std::hex << reinterpret_cast<uintptr_t>(value);
            return out.str();
        }

        static const char* SafeString(const char* value)
        {
            return value ? value : "";
        }

        static std::string PublicKeyToken(const Il2CppAssemblyName& name)
        {
            bool hasToken = false;
            for (size_t i = 0; i < sizeof(name.public_key_token); ++i)
                hasToken = hasToken || name.public_key_token[i] != 0;
            if (!hasToken)
                return "null";
            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (size_t i = 0; i < sizeof(name.public_key_token); ++i)
                out << std::setw(2) << static_cast<unsigned int>(name.public_key_token[i]);
            return out.str();
        }

        static std::string DiagnosticAssemblyNameToString(const Il2CppAssemblyName& name)
        {
            std::string fullName = il2cpp::vm::AssemblyName::AssemblyNameToString(name);
            const std::string token = PublicKeyToken(name);
            if (token == "null")
                return fullName;

            const std::string marker = ", PublicKeyToken=";
            const size_t valueStart = fullName.find(marker);
            if (valueStart == std::string::npos)
                return fullName;
            const size_t tokenStart = valueStart + marker.size();
            const size_t tokenEnd = fullName.find(',', tokenStart);
            fullName.replace(tokenStart,
                tokenEnd == std::string::npos ? std::string::npos : tokenEnd - tokenStart,
                token);
            return fullName;
        }

        static uint32_t InterpreterImageId(const Il2CppImage* image)
        {
            return image && metadata::IsInterpreterImage(image)
                ? metadata::DecodeImageIndex(image->token) : 0;
        }

        static bool ReadPublishedMvid(const Il2CppImage* image, uint32_t imageId, std::string& mvid)
        {
            if (!image || !imageId)
                return false;
            metadata::InterpreterImage* published = metadata::InterpreterMetadataIndexRuntime::GetPublishedImage(imageId);
            // This identity observation must never walk MetadataModule: that
            // API can return a private staged image during validation.
            if (!published || published->GetIl2CppImage() != image)
                return false;
            if (published->GetRawImage().GetTableRowNum(metadata::TableType::MODULE) != 1)
                return false;
            metadata::TbModule module = published->GetRawImage().ReadModule(1);
            return published->GetRawImage().TryReadGuid(module.mvid, mvid);
        }

        static void AppendAssemblyIdentity(std::ostringstream& out, const Il2CppAssembly* assembly,
            bool comma, const Il2CppImage* forcedImage = nullptr, uint32_t forcedImageId = 0,
            bool published = false)
        {
            const Il2CppImage* image = forcedImage ? forcedImage : (assembly ? assembly->image : nullptr);
            const Il2CppAssemblyName emptyName = {};
            const Il2CppAssemblyName& name = assembly ? assembly->aname : emptyName;
            const char* assemblyName = assembly ? SafeString(name.name) : "<unbound>";
            const std::string fullName = assembly ? DiagnosticAssemblyNameToString(name) : "<unbound>";
            const uint32_t imageId = forcedImageId ? forcedImageId : InterpreterImageId(image);
            std::string mvid;
            const bool mvidAvailable = ReadPublishedMvid(image, imageId, mvid);
            if (comma)
                out << ',';
            out << "{\"nativeAssemblyId\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(NativePointer(assembly))
                << ",\"nativeImageId\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(NativePointer(image))
                << ",\"name\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(assemblyName)
                << ",\"fullName\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(fullName)
                << ",\"versionMajor\":" << name.major
                << ",\"versionMinor\":" << name.minor
                << ",\"versionBuild\":" << name.build
                << ",\"versionRevision\":" << name.revision
                << ",\"culture\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(SafeString(name.culture))
                << ",\"flags\":" << name.flags
                << ",\"publicKeyToken\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(PublicKeyToken(name))
                << ",\"mvidAvailable\":" << (mvidAvailable ? "true" : "false")
                << ",\"mvid\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(mvid)
                << ",\"imageKind\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(imageId ? "Interpreter" : "Aot")
                << ",\"imageId\":" << imageId
                << ",\"published\":" << (published || mvidAvailable ? "true" : "false")
                << ",\"identityKey\":" << il2cpp::vm::AssemblyShadowDiagnostics::Quote(
                    std::string(assemblyName) + "|" + (mvidAvailable ? mvid : "<unavailable>") +
                    "|" + fullName +
                    "|" + NativePointer(assembly) + "|" + NativePointer(image) +
                    "|" + (imageId ? "Interpreter" : "Aot") + "|" + std::to_string(imageId))
                << '}';
        }

        static void AppendAssemblyArray(std::ostringstream& out, const char* field,
            const AssemblyVector& assemblies)
        {
            out << ",\"" << field << "\":[";
            for (size_t i = 0; i < assemblies.size(); ++i)
                AppendAssemblyIdentity(out, assemblies[i], i != 0);
            out << ']';
        }

        static void AppendPublishedArray(std::ostringstream& out, const char* field)
        {
            out << ",\"" << field << "\":[";
            bool comma = false;
            for (uint32_t imageId = 1; imageId <= metadata::InterpreterMetadataIndexRuntime::Codec::kMaxImageCount; ++imageId)
            {
                metadata::InterpreterImage* published = metadata::InterpreterMetadataIndexRuntime::GetPublishedImage(imageId);
                if (!published || published->GetIndex() != imageId)
                    continue;
                const Il2CppImage* image = published->GetIl2CppImage();
                const Il2CppAssembly* assembly = image ? image->assembly : nullptr;
                AppendAssemblyIdentity(out, assembly, comma, image, imageId, true);
                comma = true;
            }
            out << ']';
        }
    }

    int32_t RuntimeApi::GetH1CountDiagnosticsJson(Il2CppString** json)
    {
        if (json)
            *json = nullptr;
        if (!json)
            return static_cast<int32_t>(il2cpp::vm::AssemblyShadowError::InvalidArgument);

        try
        {
            il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
            AssemblyVector logicalAssemblies;
            AssemblyVector physicalAssemblies;
            il2cpp::vm::Assembly::GetAllAssemblies(logicalAssemblies);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
            il2cpp::vm::Assembly::GetAllPhysicalAssemblies(physicalAssemblies);
#else
            physicalAssemblies = logicalAssemblies;
#endif
            using IndexRuntime = metadata::InterpreterMetadataIndexRuntime;
            using Codec = IndexRuntime::Codec;
            Codec::Stats stats{};
            uint64_t ordinary = 0;
            uint64_t shadow = 0;
            uint64_t reserved = 0;
            if (metadata::InterpreterImage::GetMetadataCapacitySnapshotLocked(stats, ordinary, shadow, reserved) != IndexRuntime::Error::None)
                return static_cast<int32_t>(il2cpp::vm::AssemblyShadowError::InternalError);

#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
            const char* featureMode = "AssemblyShadowOn";
            const char* featureEnabled = "true";
#else
            const char* featureMode = "AssemblyShadowOff";
            const char* featureEnabled = "false";
#endif
            std::ostringstream out;
            out << "{\"schemaVersion\":1"
                << ",\"kind\":\"H1CountNativeDiagnostics\""
                << ",\"diagnosticOnly\":true"
                << ",\"featureEnabled\":" << featureEnabled
                << ",\"featureMode\":\"" << featureMode << "\""
                << ",\"reservedPages\":" << stats.reservedPages
                << ",\"mappedPages\":" << stats.mappedPages
                << ",\"reservationCount\":" << stats.reservationCount
                << ",\"nextImageId\":" << stats.nextImageId
                << ",\"nextPageSlot\":" << stats.nextPageSlot
                << ",\"ordinaryAllocatedCount\":" << ordinary
                << ",\"shadowAllocatedCount\":" << shadow
                << ",\"reservedImageCount\":" << reserved
                ;
            AppendAssemblyArray(out, "logicalAssemblies", logicalAssemblies);
            AppendAssemblyArray(out, "physicalAssemblies", physicalAssemblies);
            AppendPublishedArray(out, "publishedInterpreterImages");
            out << '}';
            const std::string serialized = out.str();
            Il2CppString* result = il2cpp::vm::String::New(serialized.c_str());
            if (!result)
                return static_cast<int32_t>(il2cpp::vm::AssemblyShadowError::InternalError);
            *json = result;
            return static_cast<int32_t>(il2cpp::vm::AssemblyShadowError::Success);
        }
        catch (const std::exception&)
        {
            return static_cast<int32_t>(il2cpp::vm::AssemblyShadowError::InternalError);
        }
        catch (...)
        {
            return static_cast<int32_t>(il2cpp::vm::AssemblyShadowError::InternalError);
        }
    }
#endif
}
