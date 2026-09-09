#pragma once

#include <cstdint>

namespace hybridclr
{
namespace metadata
{
    class BlobReaderBounds
    {
    public:
        static bool TryReadCompressedUint32(const uint8_t* buf, uint32_t available, uint32_t& value, uint32_t& lengthSize)
        {
            if (buf == nullptr || available == 0)
            {
                return false;
            }

            uint32_t firstByte = buf[0];
            if (firstByte < 128)
            {
                lengthSize = 1;
                value = firstByte;
                return true;
            }
            if (firstByte < 192)
            {
                if (available < 2)
                {
                    return false;
                }
                lengthSize = 2;
                value = ((firstByte & 0x3f) << 8) | buf[1];
                return true;
            }
            if (firstByte < 224)
            {
                if (available < 4)
                {
                    return false;
                }
                lengthSize = 4;
                value = ((firstByte & 0x1f) << 24) | (((uint32_t)buf[1]) << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
                return true;
            }
            return false;
        }

        static bool TryReadLengthPrefixedBlob(const uint8_t* buf, uint32_t available, const uint8_t*& data, uint32_t& length)
        {
            uint32_t lengthSize;
            if (!TryReadCompressedUint32(buf, available, length, lengthSize) || length > available - lengthSize)
            {
                return false;
            }
            data = buf + lengthSize;
            return true;
        }

        static bool ValidateLengthPrefixedHeap(const uint8_t* buf, uint32_t size)
        {
            if (buf == nullptr && size != 0)
                return false;
            uint32_t offset = 0;
            while (offset < size)
            {
                const uint8_t* data;
                uint32_t length;
                const uint32_t available = size - offset;
                const uint8_t* entry = buf + offset;
                if (!TryReadLengthPrefixedBlob(entry, available, data, length))
                    return false;
                offset += static_cast<uint32_t>(data - entry) + length;
            }
            return true;
        }
    };
}
}
