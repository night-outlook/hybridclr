#pragma once

#include <cstddef>
#include <cstdint>

#include "InterpreterMetadataIndexCodec.h"

namespace hybridclr
{
namespace metadata
{
    // Profile-2 preliminary admission only. Final page demand is computed and
    // sealed by the runtime after private metadata initialization.
    struct InterpreterImageAdmission
    {
        static constexpr uint32_t kProfileVersion = 2;
        static constexpr uint64_t kMaximumDllBytes = uint64_t(32) * 1024 * 1024;
        static constexpr uint32_t kMaximumImages = InterpreterMetadataIndexCodec::kMaxImageCount;

        enum class Error : uint8_t
        {
            None,
            InvalidState,
            InvalidInput,
            EmptyDll,
            DllTooLarge,
            ImageLimit,
        };

        struct Report
        {
            Error error;
            size_t firstFailureIndex;
            uint32_t reservedImageCountBefore;
            uint32_t reservedImageCountAfter;
            bool runtimeFinalizationRequired;

            bool IsSuccess() const { return error == Error::None; }
        };

        // Retained failed reservations are already included in reservedImages.
        // No input-byte sum is interpreted as metadata RAM or encoded capacity.
        // A failed proposal leaves the reported committed count unchanged.
        static Report Evaluate(uint32_t reservedImages, const uint64_t* dllSizes,
            size_t count)
        {
            Report report = {Error::None, count, reservedImages, reservedImages, true};
            if (reservedImages > kMaximumImages)
            {
                report.error = Error::InvalidState;
                report.firstFailureIndex = 0;
                return report;
            }
            if (count != 0 && dllSizes == nullptr)
            {
                report.error = Error::InvalidInput;
                report.firstFailureIndex = 0;
                return report;
            }
            const uint32_t remaining = kMaximumImages - reservedImages;
            if (count > remaining)
            {
                report.error = Error::ImageLimit;
                report.firstFailureIndex = remaining;
                return report;
            }
            for (size_t index = 0; index < count; ++index)
            {
                if (dllSizes[index] == 0 || dllSizes[index] > kMaximumDllBytes)
                {
                    report.error = dllSizes[index] == 0 ? Error::EmptyDll : Error::DllTooLarge;
                    report.firstFailureIndex = index;
                    return report;
                }
            }
            report.reservedImageCountAfter = reservedImages + static_cast<uint32_t>(count);
            return report;
        }
    };
}
}
