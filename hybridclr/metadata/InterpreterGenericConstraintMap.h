#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "vm/GlobalMetadataFileInternals.h"

namespace hybridclr
{
namespace metadata
{
    // The native Il2CppGenericParameter field remains int16_t.  This helper
    // owns only interpreter raw-row mapping and validation; lazy type-index
    // resolution remains in InterpreterImage.
    struct InterpreterGenericConstraintMap
    {
        static uint32_t InvalidStart()
        {
            return std::numeric_limits<uint32_t>::max();
        }

        enum class Error
        {
            None,
            InvalidOwner,
            NonContiguousOwner,
            CountOverflow,
            InvalidHandle,
            ForeignHandle,
            InvalidOrdinal,
            InvalidSidecar,
            InvalidRawRange,
        };

        static void Initialize(std::vector<uint32_t>& starts, size_t parameterCount)
        {
            starts.assign(parameterCount, InvalidStart());
        }

        static Error RecordRow(uint32_t owner, uint32_t rawRowIndex,
            std::vector<Il2CppGenericParameter>& parameters,
            std::vector<uint32_t>& starts, int32_t& lastOwner)
        {
            if (owner == 0 || owner > parameters.size() || starts.size() != parameters.size())
                return Error::InvalidOwner;
            const uint32_t ownerIndex = owner - 1;
            if (static_cast<int32_t>(ownerIndex) != lastOwner)
            {
                if (starts[ownerIndex] != InvalidStart())
                    return Error::NonContiguousOwner;
                starts[ownerIndex] = rawRowIndex;
                lastOwner = static_cast<int32_t>(ownerIndex);
            }
            if (parameters[ownerIndex].constraintsCount >= std::numeric_limits<GenericParameterConstraintIndex>::max())
                return Error::CountOverflow;
            ++parameters[ownerIndex].constraintsCount;
            return Error::None;
        }

        static Error ResolveRawIndex(const Il2CppGenericParameter* handle,
            uint32_t imageIndex, uint32_t (*decodeImageIndex)(int32_t),
            const std::vector<Il2CppGenericParameter>& parameters,
            const std::vector<uint32_t>& starts, size_t rawConstraintCount,
            GenericParameterConstraintIndex ordinal, uint32_t& rawIndex)
        {
            if (handle == nullptr || parameters.empty())
                return Error::InvalidHandle;
            const uintptr_t address = reinterpret_cast<uintptr_t>(handle);
            const uintptr_t begin = reinterpret_cast<uintptr_t>(parameters.data());
            const uintptr_t end = begin + parameters.size() * sizeof(Il2CppGenericParameter);
            if (address < begin || address >= end ||
                (address - begin) % sizeof(Il2CppGenericParameter) != 0)
                return Error::ForeignHandle;
            if (decodeImageIndex == nullptr || decodeImageIndex(handle->ownerIndex) != imageIndex)
                return Error::ForeignHandle;
            if (ordinal < 0 || ordinal >= handle->constraintsCount)
                return Error::InvalidOrdinal;

            const size_t parameterIndex = (address - begin) / sizeof(Il2CppGenericParameter);
            if (parameterIndex >= starts.size())
                return Error::InvalidSidecar;
            const uint32_t rawStart = starts[parameterIndex];
            if (rawStart == InvalidStart() || rawStart > rawConstraintCount ||
                static_cast<uint32_t>(ordinal) > rawConstraintCount - rawStart ||
                rawStart > std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(ordinal))
                return Error::InvalidRawRange;
            rawIndex = rawStart + static_cast<uint32_t>(ordinal);
            return rawIndex < rawConstraintCount ? Error::None : Error::InvalidRawRange;
        }
    };
}
}
