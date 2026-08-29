#include "AssemblyShadowRuntimeApi.h"

#include "vm/Array.h"
#include "vm/AssemblyShadow.h"
#include "vm/InternalCalls.h"
#include "vm/String.h"
#include "utils/StringUtils.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace hybridclr
{
    using il2cpp::vm::AssemblyShadowError;

    namespace
    {
        static int32_t ErrorCode(AssemblyShadowError error)
        {
            return static_cast<int32_t>(error);
        }

        static bool ReadString(Il2CppString* value, std::string& result)
        {
            if (!value)
                return false;

            for (int32_t i = 0; i < value->length; ++i)
            {
                if (value->chars[i] == 0)
                    return false;
            }

            result = il2cpp::utils::StringUtils::Utf16ToUtf8(value->chars, value->length);
            return true;
        }

        static bool ReadStringArray(Il2CppArray* values, std::vector<std::string>& result)
        {
            if (!values)
                return false;

            const uint32_t length = il2cpp::vm::Array::GetLength(values);
            Il2CppString** entries = reinterpret_cast<Il2CppString**>(il2cpp::vm::Array::GetFirstElementAddress(values));
            result.clear();
            result.reserve(length);
            for (uint32_t i = 0; i < length; ++i)
            {
                std::string value;
                if (!ReadString(entries[i], value))
                    return false;
                result.push_back(std::move(value));
            }
            return true;
        }

        static bool ReadByteArray(Il2CppArray* values, const uint8_t*& bytes, size_t& length, bool allowNull)
        {
            if (!values)
            {
                bytes = nullptr;
                length = 0;
                return allowNull;
            }

            length = static_cast<size_t>(il2cpp::vm::Array::GetByteLength(values));
            bytes = reinterpret_cast<const uint8_t*>(il2cpp::vm::Array::GetFirstElementAddress(values));
            return true;
        }

        static int32_t Disabled()
        {
            return ErrorCode(AssemblyShadowError::FeatureDisabled);
        }

        static int32_t UnexpectedMutationFailure()
        {
            return ErrorCode(il2cpp::vm::AssemblyShadow::ReportUnexpectedFailure());
        }
    }

    void AssemblyShadowRuntimeApi::RegisterInternalCalls()
    {
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::ConfigureCandidates(System.String,System.String[],System.String[])", (Il2CppMethodPointer)ConfigureCandidates);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::BeginTransaction(System.String,System.String,System.String[],System.Int32)", (Il2CppMethodPointer)BeginTransaction);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::StageAssembly(System.Byte[],System.Byte[])", (Il2CppMethodPointer)StageAssembly);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::ValidateTransaction()", (Il2CppMethodPointer)ValidateTransaction);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::CommitTransaction()", (Il2CppMethodPointer)CommitTransaction);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::AbortTransaction()", (Il2CppMethodPointer)AbortTransaction);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::GetState(HybridCLR.AssemblyShadowState&)", (Il2CppMethodPointer)GetState);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::GetAssemblyExecutionMode(System.String,HybridCLR.AssemblyExecutionMode&)", (Il2CppMethodPointer)GetAssemblyExecutionMode);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::GetDiagnosticsJson(System.String&)", (Il2CppMethodPointer)GetDiagnosticsJson);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::GetTypeResolutionInfo(System.Type,System.String&)", (Il2CppMethodPointer)GetTypeResolutionInfo);
        il2cpp::vm::InternalCalls::Add("HybridCLR.AssemblyShadowRuntime::GetExecutionDiagnosticsJson(System.String&)", (Il2CppMethodPointer)GetExecutionDiagnosticsJson);
    }

    int32_t AssemblyShadowRuntimeApi::ConfigureCandidates(Il2CppString* baselineBuildId,
        Il2CppArray* candidateAssemblyNames, Il2CppArray* stableAotNames)
    {
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        try
        {
            std::string baseline;
            std::vector<std::string> candidates;
            std::vector<std::string> stable;
            if (!ReadString(baselineBuildId, baseline) ||
                !ReadStringArray(candidateAssemblyNames, candidates) ||
                !ReadStringArray(stableAotNames, stable))
                return ErrorCode(AssemblyShadowError::InvalidArgument);
            return ErrorCode(il2cpp::vm::AssemblyShadow::ConfigureCandidates(baseline.c_str(), candidates, stable));
        }
        catch (const std::exception&)
        {
            return UnexpectedMutationFailure();
        }
        catch (...)
        {
            return UnexpectedMutationFailure();
        }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::BeginTransaction(Il2CppString* patchId,
        Il2CppString* expectedBaselineBuildId, Il2CppArray* closureLoadOrder,
        int32_t runtimeAbiVersion)
    {
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        try
        {
            std::string patch;
            std::string expectedBaseline;
            std::vector<std::string> closure;
            if (!ReadString(patchId, patch) || !ReadString(expectedBaselineBuildId, expectedBaseline) ||
                !ReadStringArray(closureLoadOrder, closure))
                return ErrorCode(AssemblyShadowError::InvalidArgument);
            return ErrorCode(il2cpp::vm::AssemblyShadow::BeginTransaction(
                patch.c_str(), expectedBaseline.c_str(), closure, runtimeAbiVersion));
        }
        catch (const std::exception&)
        {
            return UnexpectedMutationFailure();
        }
        catch (...)
        {
            return UnexpectedMutationFailure();
        }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::StageAssembly(Il2CppArray* dllBytes, Il2CppArray* pdbBytes)
    {
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        try
        {
            const uint8_t* dll = nullptr;
            size_t dllLength = 0;
            const uint8_t* pdb = nullptr;
            size_t pdbLength = 0;
            if (!ReadByteArray(dllBytes, dll, dllLength, false) || dllLength == 0 ||
                !ReadByteArray(pdbBytes, pdb, pdbLength, true))
                return ErrorCode(AssemblyShadowError::InvalidArgument);
            return ErrorCode(il2cpp::vm::AssemblyShadow::StageAssembly(dll, dllLength, pdb, pdbLength));
        }
        catch (const std::exception&)
        {
            return UnexpectedMutationFailure();
        }
        catch (...)
        {
            return UnexpectedMutationFailure();
        }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::ValidateTransaction()
    {
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        try { return ErrorCode(il2cpp::vm::AssemblyShadow::ValidateTransaction()); }
        catch (const std::exception&) { return UnexpectedMutationFailure(); }
        catch (...) { return UnexpectedMutationFailure(); }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::CommitTransaction()
    {
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        try { return ErrorCode(il2cpp::vm::AssemblyShadow::CommitTransaction()); }
        catch (const std::exception&) { return UnexpectedMutationFailure(); }
        catch (...) { return UnexpectedMutationFailure(); }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::AbortTransaction()
    {
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        try { return ErrorCode(il2cpp::vm::AssemblyShadow::AbortTransaction()); }
        catch (const std::exception&) { return UnexpectedMutationFailure(); }
        catch (...) { return UnexpectedMutationFailure(); }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::GetState(int32_t* state)
    {
        if (state)
            *state = static_cast<int32_t>(il2cpp::vm::AssemblyShadowState::Disabled);
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        if (!state)
            return ErrorCode(AssemblyShadowError::InvalidArgument);
        try
        {
            il2cpp::vm::AssemblyShadowState value = il2cpp::vm::AssemblyShadowState::Disabled;
            AssemblyShadowError result = il2cpp::vm::AssemblyShadow::GetState(value);
            *state = static_cast<int32_t>(value);
            return ErrorCode(result);
        }
        catch (const std::exception&)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
        catch (...)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::GetAssemblyExecutionMode(Il2CppString* logicalAssemblyName, int32_t* mode)
    {
        if (mode)
            *mode = static_cast<int32_t>(il2cpp::vm::AssemblyExecutionMode::AotBaseline);
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        if (!mode)
            return ErrorCode(AssemblyShadowError::InvalidArgument);
        try
        {
            std::string name;
            if (!ReadString(logicalAssemblyName, name))
                return ErrorCode(AssemblyShadowError::InvalidArgument);
            il2cpp::vm::AssemblyExecutionMode value = il2cpp::vm::AssemblyExecutionMode::AotBaseline;
            AssemblyShadowError result = il2cpp::vm::AssemblyShadow::GetAssemblyExecutionMode(name.c_str(), value);
            *mode = static_cast<int32_t>(value);
            return ErrorCode(result);
        }
        catch (const std::exception&)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
        catch (...)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::GetTypeResolutionInfo(Il2CppReflectionType* type, Il2CppString** json)
    {
        if (json)
            *json = nullptr;
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        if (!json || !type || !type->type)
            return ErrorCode(AssemblyShadowError::InvalidArgument);
        try
        {
            std::string value;
            AssemblyShadowError result = il2cpp::vm::AssemblyShadow::GetTypeResolutionInfo(type->type, value);
            if (!value.empty())
                *json = il2cpp::vm::String::New(value.c_str());
            return ErrorCode(result);
        }
        catch (const std::exception&)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
        catch (...)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::GetExecutionDiagnosticsJson(Il2CppString** json)
    {
        if (json) *json = nullptr;
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        return Disabled();
#else
        if (!json) return ErrorCode(AssemblyShadowError::InvalidArgument);
        try
        {
            std::string value;
            AssemblyShadowError result = il2cpp::vm::AssemblyShadow::GetExecutionDiagnosticsJson(value);
            if (result == AssemblyShadowError::Success) *json = il2cpp::vm::String::New(value.c_str());
            return ErrorCode(result);
        }
        catch (...) { return ErrorCode(AssemblyShadowError::InternalError); }
#endif
    }

    int32_t AssemblyShadowRuntimeApi::GetDiagnosticsJson(Il2CppString** json)
    {
        if (json)
            *json = nullptr;
#if !HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
        if (!json)
            return Disabled();
#else
        if (!json)
            return ErrorCode(AssemblyShadowError::InvalidArgument);
#endif
        try
        {
            std::string value;
            AssemblyShadowError result = il2cpp::vm::AssemblyShadow::GetDiagnosticsJson(value);
            // The native serializer owns the schema in both feature modes.
            // FeatureDisabled still carries a complete diagnostic snapshot.
            if (!value.empty())
                *json = il2cpp::vm::String::New(value.c_str());
            return ErrorCode(result);
        }
        catch (const std::exception&)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
        catch (...)
        {
            return ErrorCode(AssemblyShadowError::InternalError);
        }
    }
}
