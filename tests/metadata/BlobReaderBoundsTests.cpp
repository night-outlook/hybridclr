#include "hybridclr/metadata/BlobReaderBounds.h"

#include <cstdint>
#include <iostream>

using hybridclr::metadata::BlobReaderBounds;

namespace
{
    bool Rejects(const uint8_t* input, uint32_t available)
    {
        const uint8_t* data = nullptr;
        uint32_t length = 0;
        return !BlobReaderBounds::TryReadLengthPrefixedBlob(input, available, data, length);
    }

    bool Accepts(const uint8_t* input, uint32_t available, uint32_t expectedLength)
    {
        const uint8_t* data = nullptr;
        uint32_t length = 0;
        return BlobReaderBounds::TryReadLengthPrefixedBlob(input, available, data, length)
            && data == input + 1
            && length == expectedLength;
    }

    bool ValidatesHeap(const uint8_t* input, uint32_t size)
    {
        return BlobReaderBounds::ValidateLengthPrefixedHeap(input, size);
    }
}

int main()
{
    const uint8_t truncatedTwoBytePrefix[] = { 0x80 };
    const uint8_t truncatedFourBytePrefix[] = { 0xc0, 0x00, 0x00 };
    const uint8_t oversizedDeclaredLength[] = { 0x03, 0xaa, 0xbb };
    const uint8_t oversizedFourByteDeclaredLength[] = { 0xc0, 0x00, 0x00, 0x04, 0xaa };
    const uint8_t completeBlob[] = { 0x02, 0xaa, 0xbb };
    const uint8_t userStringHeap[] = { 0x01, 'u', 0x00, 0x02, 'o', 'k' };
    const uint8_t blobHeap[] = { 0x00, 0x01, 0xff };

    if (!Rejects(truncatedTwoBytePrefix, sizeof(truncatedTwoBytePrefix)))
    {
        std::cerr << "accepted truncated two-byte compressed prefix\n";
        return 1;
    }
    if (!Rejects(truncatedFourBytePrefix, sizeof(truncatedFourBytePrefix)))
    {
        std::cerr << "accepted truncated four-byte compressed prefix\n";
        return 1;
    }
    if (!Rejects(oversizedDeclaredLength, sizeof(oversizedDeclaredLength)))
    {
        std::cerr << "accepted length larger than remaining payload\n";
        return 1;
    }
    if (!Rejects(oversizedFourByteDeclaredLength, sizeof(oversizedFourByteDeclaredLength)))
    {
        std::cerr << "accepted four-byte length larger than remaining payload\n";
        return 1;
    }
    if (!Accepts(completeBlob, sizeof(completeBlob), 2))
    {
        std::cerr << "rejected valid length-prefixed blob\n";
        return 1;
    }
    if (!ValidatesHeap(userStringHeap, sizeof(userStringHeap)))
    {
        std::cerr << "rejected valid #US heap\n";
        return 1;
    }
    if (!ValidatesHeap(blobHeap, sizeof(blobHeap)))
    {
        std::cerr << "rejected valid #Blob heap\n";
        return 1;
    }
    if (!ValidatesHeap(nullptr, 0))
    {
        std::cerr << "rejected absent #Blob heap\n";
        return 1;
    }
    if (ValidatesHeap(nullptr, sizeof(completeBlob)))
    {
        std::cerr << "accepted non-empty heap without data\n";
        return 1;
    }
    if (ValidatesHeap(oversizedDeclaredLength, sizeof(oversizedDeclaredLength)))
    {
        std::cerr << "accepted malformed #US/#Blob heap\n";
        return 1;
    }
    return 0;
}
