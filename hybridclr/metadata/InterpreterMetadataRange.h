#pragma once

#include <cstdint>
#include <limits>

namespace hybridclr
{
namespace metadata
{
    // Checked arithmetic for metadata fields whose values are encoded with an
    // image owner.  Callers must decode before doing range arithmetic: encoded
    // image bits are not part of the raw table coordinate space.
    struct InterpreterMetadataRange
    {
        struct DecodedIndex
        {
            bool valid;
            uint32_t owner;
            uint64_t raw;

            DecodedIndex() : valid(false), owner(0), raw(0) {}
            DecodedIndex(bool isValid, uint32_t imageOwner, uint64_t rawIndex)
                : valid(isValid), owner(imageOwner), raw(rawIndex) {}
        };

        enum class Error : uint8_t
        {
            None = 0,
            InvalidArgument,
            InvalidEncoding,
            ForeignOwner,
            OutOfRange,
            Reversed,
            CountOverflow,
        };

        template <typename Decoder>
        static Error DecodeEndpoint(uint32_t encoded, uint32_t expectedOwner,
            uint32_t rawLimit, const Decoder& decoder, uint32_t& raw)
        {
            if (expectedOwner == 0)
                return Error::InvalidArgument;
            const DecodedIndex decoded = decoder(encoded);
            if (!decoded.valid)
                return Error::InvalidEncoding;
            if (decoded.owner != expectedOwner)
                return Error::ForeignOwner;
            if (decoded.raw > rawLimit || decoded.raw > std::numeric_limits<uint32_t>::max())
                return Error::OutOfRange;
            raw = static_cast<uint32_t>(decoded.raw);
            return Error::None;
        }

        static Error CountRawRange(uint32_t begin, uint32_t end,
            uint32_t maxCount, uint32_t& count)
        {
            if (end < begin)
                return Error::Reversed;
            const uint32_t length = end - begin;
            if (length > maxCount)
                return Error::CountOverflow;
            count = length;
            return Error::None;
        }

        static uint32_t CompressedUint32Size(uint32_t value)
        {
            if (value < 0x80)
                return 1;
            if (value < 0x4000)
                return 2;
            if (value < 0x20000000)
                return 4;
            return value < std::numeric_limits<uint32_t>::max() - 1u ? 5 : 1;
        }

        // CustomAttributeDataWriter::Skip takes int32_t and the method-index
        // area is addressed through int32_t offsets. Validate the complete
        // compressed-count prefix plus method-index area before writing either.
        static Error ValidateCustomAttributeLayout(uint32_t begin, uint32_t end,
            uint32_t count, uint32_t writerSize, uint32_t& methodIndexBytes)
        {
            if (count == 0 || end < begin || end - begin != count)
                return Error::InvalidArgument;
            if (begin > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
                end > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
                writerSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                return Error::OutOfRange;

            const uint64_t prefixBytes = CompressedUint32Size(count);
            const uint64_t indexBytes = uint64_t(count) * sizeof(int32_t);
            const uint64_t totalBytes = uint64_t(writerSize) + prefixBytes + indexBytes;
            if (totalBytes > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
                return Error::OutOfRange;
            methodIndexBytes = static_cast<uint32_t>(indexBytes);
            return Error::None;
        }

        template <typename Decoder>
        static Error DecodeRange(uint32_t encodedBegin, uint32_t encodedEnd,
            uint32_t expectedOwner, uint32_t rawLimit, uint32_t maxCount,
            const Decoder& decoder, uint32_t& begin, uint32_t& end,
            uint32_t& count)
        {
            Error error = DecodeEndpoint(encodedBegin, expectedOwner, rawLimit, decoder, begin);
            if (error != Error::None)
                return error;
            error = DecodeEndpoint(encodedEnd, expectedOwner, rawLimit, decoder, end);
            if (error != Error::None)
                return error;
            return CountRawRange(begin, end, maxCount, count);
        }

        template <typename Decoder>
        static Error ResolveOffset(uint32_t encodedBase, uint32_t expectedOwner,
            uint32_t localOffset, uint32_t rawLimit, const Decoder& decoder,
            uint32_t& raw)
        {
            uint32_t base = 0;
            Error error = DecodeEndpoint(encodedBase, expectedOwner, rawLimit, decoder, base);
            if (error != Error::None)
                return error;
            if (localOffset >= rawLimit - base)
                return Error::OutOfRange;
            raw = base + localOffset;
            return Error::None;
        }
    };
}
}
