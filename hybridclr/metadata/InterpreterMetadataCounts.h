#pragma once

#include <cstdint>
#include <limits>

namespace hybridclr
{
namespace metadata
{
    // Native per-type counts remain uint16_t. A count of 65535 permits
    // slots 0..65534, leaving 65535 available as the invalid method slot.
    struct InterpreterMetadataCounts
    {
        static bool CanAppend(uint64_t current, uint64_t added)
        {
            const uint64_t maximum = std::numeric_limits<uint16_t>::max();
            return current <= maximum && added <= maximum - current;
        }

        // ECMA list columns are one-based and may point one past the table.
        // Do all validation before assigning the narrow destination.
        static bool TryListRange(uint64_t begin, uint64_t end,
            uint32_t rowCount, uint16_t& count)
        {
            if (begin == 0 || end < begin || end > uint64_t(rowCount) + 1 ||
                !CanAppend(0, end - begin))
                return false;
            count = static_cast<uint16_t>(end - begin);
            return true;
        }
    };
}
}
