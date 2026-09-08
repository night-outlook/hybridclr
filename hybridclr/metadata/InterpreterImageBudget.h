#pragma once

// The image-index allocator is used by both ordinary interpreter loads and
// Assembly Shadow staging.  Keep this header independent of IL2CPP so that
// the exact admission rules can be dry-run by build tools and native tests.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace hybridclr
{
namespace metadata
{

struct InterpreterImageBudget
{
    // Version the encoding profile independently of the runtime ABI version.
    static constexpr uint32_t kProfileVersion = 1;

    // The low 22 bits are the raw metadata index.  Two of the ten image-index
    // bits encode the allocation kind, leaving an eight-bit image slot.
    static constexpr uint32_t kMetadataIndexBits = 22;
    static constexpr uint32_t kMetadataKindBits = 2;
    static constexpr uint32_t kMetadataImageIndexBits = 32 - kMetadataIndexBits;
    static constexpr uint32_t kMetadataImageIndexKindShift =
        kMetadataImageIndexBits - kMetadataKindBits;
    static constexpr uint32_t kMetadataKindCount = 1u << kMetadataKindBits;
    static constexpr uint32_t kMaxMetadataImageIndexWithoutKind =
        1u << (kMetadataImageIndexBits - kMetadataKindBits);
    static constexpr uint32_t kInvalidImageIndex = 0;

    // A kind's extra bits enlarge its per-image metadata-index range, and also
    // determine the spacing between image cursors in that kind's bucket.
    static constexpr uint32_t kMetadataImageIndexExtraShiftBitsA = 6;
    static constexpr uint32_t kMetadataImageIndexExtraShiftBitsB = 4;
    static constexpr uint32_t kMetadataImageIndexExtraShiftBitsC = 2;
    static constexpr uint32_t kMetadataImageIndexExtraShiftBitsD = 0;

    static constexpr uint32_t kMetadataIndexMaskA =
        (1u << (kMetadataIndexBits + kMetadataImageIndexExtraShiftBitsA)) - 1u;
    static constexpr uint32_t kMetadataIndexMaskB =
        (1u << (kMetadataIndexBits + kMetadataImageIndexExtraShiftBitsB)) - 1u;
    static constexpr uint32_t kMetadataIndexMaskC =
        (1u << (kMetadataIndexBits + kMetadataImageIndexExtraShiftBitsC)) - 1u;
    static constexpr uint32_t kMetadataIndexMaskD =
        (1u << (kMetadataIndexBits + kMetadataImageIndexExtraShiftBitsD)) - 1u;

    static constexpr uint32_t kSizeMultiplier = 4;
    static constexpr uint32_t kKind3ReservedImageIndex =
        kMaxMetadataImageIndexWithoutKind - 1u;
    static constexpr uint32_t kFreshKind0Cursor = 1u << kMetadataImageIndexExtraShiftBitsA;

    enum class Error : uint8_t
    {
        None = 0,
        InvalidSize,
        Overflow,
        Oversize,
        InvalidState,
        Exhausted,
    };

    struct State
    {
        uint32_t cursors[kMetadataKindCount];
    };

    struct Allocation
    {
        uint32_t imageIndex;
        int32_t kind;
        Error error;
        bool succeeded;

        bool IsSuccess() const
        {
            return succeeded && error == Error::None;
        }
    };

    struct Evaluation
    {
        std::vector<Allocation> allocations;
        State finalState;
        size_t firstFailureIndex;
        uint64_t firstFailureSize;
        Error firstFailure;
        bool succeeded;

        bool IsSuccess() const
        {
            return succeeded && firstFailure == Error::None;
        }
    };

    static State FreshState()
    {
        State state = {{kFreshKind0Cursor, 0u, 0u, 0u}};
        return state;
    }

    static uint32_t ExtraShiftBits(int32_t kind)
    {
        switch (kind)
        {
        case 0: return kMetadataImageIndexExtraShiftBitsA;
        case 1: return kMetadataImageIndexExtraShiftBitsB;
        case 2: return kMetadataImageIndexExtraShiftBitsC;
        case 3: return kMetadataImageIndexExtraShiftBitsD;
        default: return 0;
        }
    }

    static uint32_t IndexMask(int32_t kind)
    {
        switch (kind)
        {
        case 0: return kMetadataIndexMaskA;
        case 1: return kMetadataIndexMaskB;
        case 2: return kMetadataIndexMaskC;
        case 3: return kMetadataIndexMaskD;
        default: return 0;
        }
    }

    static uint32_t CursorStride(int32_t kind)
    {
        return 1u << ExtraShiftBits(kind);
    }

    static bool IsValidState(const State& state)
    {
        for (int32_t kind = 0; kind < static_cast<int32_t>(kMetadataKindCount); ++kind)
        {
            const uint32_t cursor = state.cursors[kind];
            const uint32_t stride = CursorStride(kind);
            const uint32_t terminal = kind == 3
                ? kKind3ReservedImageIndex
                : kMaxMetadataImageIndexWithoutKind;

            // Kind zero begins at image index 64, preserving the old ordinary
            // image range.  A terminal cursor is a valid exhausted snapshot.
            if (kind == 0 && cursor < kFreshKind0Cursor)
                return false;
            if (cursor > terminal || (cursor % stride) != 0)
                return false;
        }
        return true;
    }

    // Returns -1 for zero, multiplication overflow, and sizes outside the
    // current encoding profile.  The multiplication is guarded explicitly so
    // callers never observe a wrapped size.
    static int32_t GetImageKind(uint64_t dllSize)
    {
        if (dllSize == 0 || dllSize > std::numeric_limits<uint64_t>::max() / kSizeMultiplier)
            return -1;

        const uint64_t scaledSize = dllSize * kSizeMultiplier;
        for (int32_t kind = 3; kind >= 0; --kind)
        {
            if (scaledSize <= IndexMask(kind))
                return kind;
        }
        return -1;
    }

    static Allocation TryAllocate(State& state, uint64_t dllSize)
    {
        if (!IsValidState(state))
            return MakeFailure(Error::InvalidState);
        if (dllSize == 0)
            return MakeFailure(Error::InvalidSize);
        if (dllSize > std::numeric_limits<uint64_t>::max() / kSizeMultiplier)
            return MakeFailure(Error::Overflow);

        const uint64_t scaledSize = dllSize * kSizeMultiplier;
        if (scaledSize > kMetadataIndexMaskA)
            return MakeFailure(Error::Oversize);

        const int32_t requestedKind = GetImageKind(dllSize);
        if (requestedKind < 0)
            return MakeFailure(Error::Oversize);

        // The fallback order is part of the existing profile: when the best
        // bucket is full, a smaller kind may still represent this image.
        for (int32_t kind = requestedKind; kind >= 0; --kind)
        {
            const uint32_t cursor = state.cursors[kind];
            const uint32_t terminal = kind == 3
                ? kKind3ReservedImageIndex
                : kMaxMetadataImageIndexWithoutKind;
            if (cursor >= terminal)
                continue;

            const uint32_t stride = CursorStride(kind);
            // All valid cursors are small, but retain the explicit check so a
            // future profile cannot wrap a cursor while committing an index.
            if (cursor > std::numeric_limits<uint32_t>::max() - stride)
                continue;
            const uint32_t next = cursor + stride;
            if (next > (kind == 3 ? kKind3ReservedImageIndex : kMaxMetadataImageIndexWithoutKind))
                continue;

            const uint32_t imageIndex = cursor |
                (static_cast<uint32_t>(kind) << kMetadataImageIndexKindShift);
            if (imageIndex == kInvalidImageIndex)
                continue;

            // Commit only after every validation above has passed.  Failed
            // calls therefore leave the complete state unchanged.
            state.cursors[kind] = next;
            Allocation result = {imageIndex, kind, Error::None, true};
            return result;
        }
        return MakeFailure(Error::Exhausted);
    }

    static Evaluation Evaluate(const State& initialState, const std::vector<uint64_t>& sizes)
    {
        Evaluation result;
        result.finalState = initialState;
        result.firstFailureIndex = static_cast<size_t>(-1);
        result.firstFailureSize = 0;
        result.firstFailure = Error::None;
        result.succeeded = false;

        if (!IsValidState(initialState))
        {
            result.firstFailureIndex = 0;
            result.firstFailure = Error::InvalidState;
            return result;
        }

        State working = initialState;
        result.allocations.reserve(sizes.size());
        for (size_t index = 0; index < sizes.size(); ++index)
        {
            const uint64_t size = sizes[index];
            Allocation allocation = TryAllocate(working, size);
            if (!allocation.IsSuccess())
            {
                result.finalState = working;
                result.firstFailureIndex = index;
                result.firstFailureSize = size;
                result.firstFailure = allocation.error;
                return result;
            }
            result.allocations.push_back(allocation);
        }

        result.finalState = working;
        result.succeeded = true;
        return result;
    }

private:
    static Allocation MakeFailure(Error error)
    {
        Allocation result = {kInvalidImageIndex, -1, error, false};
        return result;
    }
};

} // namespace metadata
} // namespace hybridclr
