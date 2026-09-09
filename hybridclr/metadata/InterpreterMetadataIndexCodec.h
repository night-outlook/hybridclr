#pragma once

// Experimental, standalone sparse-page interpreter metadata index kernel.
//
// This header deliberately has no IL2CPP dependency.  It owns the process
// lifetime image/page ledger and is intended for runtime adapters to call after
// their ordinary-construction or Shadow staging ownership checks.  It does not
// replace those checks; it is not yet wired into the runtime and defines the
// standalone kernel implementation boundary while that integration proceeds.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace hybridclr
{
namespace metadata
{

class InterpreterMetadataIndexCodec
{
public:
    static constexpr uint64_t kInterpreterRawDomainLimit = uint64_t(1) << 31;
    static constexpr uint32_t kPageShift = 12;
    static constexpr uint32_t kPageSize = uint32_t(1) << kPageShift;
    static constexpr uint32_t kUsablePageCount = 524287u;
    static constexpr uint32_t kMaxImageCount = 8192u;
    static constexpr uint32_t kMaxChargedPages = 393215u;
    static constexpr uint32_t kEncodedBase = 0x80000000u;
    static constexpr uint32_t kEncodedLast =
        kEncodedBase + kUsablePageCount * kPageSize - 1u;

    enum class Error : uint8_t
    {
        None = 0,
        InvalidLimits,
        OutOfMemory,
        InvalidArgument,
        OwnerRequired,
        ZeroCredits,
        ImageLimit,
        PageLimit,
        InvalidImage,
        InvalidReservation,
        InvalidState,
        Unfinalized,
        AlreadyFinalized,
        QuotaExceeded,
        RawOutOfRange,
        InvalidFootprint,
        FootprintViolation,
        Sentinel,
        AotDomain,
        UnknownToken,
        NotOwner,
        CrossImage,
        ArithmeticOverflow,
        BindingMapFull,
    };

    struct Reservation
    {
        uint32_t imageId;
        uint64_t owner;

        Reservation() : imageId(0), owner(0) {}
        Reservation(uint32_t image, uint64_t ownerValue)
            : imageId(image), owner(ownerValue) {}
    };

    struct BatchRequest
    {
        uint64_t owner;
        uint32_t credits;
    };

    struct DecodedData
    {
        uint32_t imageId;
        int64_t rawIndex;

        DecodedData() : imageId(0), rawIndex(0) {}
    };

    struct Stats
    {
        uint64_t reservedPages;
        uint64_t mappedPages;
        uint32_t reservationCount;
        uint32_t nextImageId;
        uint32_t nextPageSlot;
    };

    struct FootprintStats
    {
        uint64_t lowEnd;
        uint64_t requiredPages;
        uint32_t chargedPages;
        uint32_t mappedPages;
        bool sealed;
    };

    explicit InterpreterMetadataIndexCodec(
        uint32_t maxImages = kMaxImageCount,
        uint32_t maxChargedPages = kMaxChargedPages,
        // Test-only deterministic constructor seam: zero fails the first
        // preallocation, one the second, and -1 disables injection.
        int32_t allocationFailureOrdinal = -1)
        : maxImages_(maxImages), maxChargedPages_(maxChargedPages),
          valid_(false), initializationError_(Error::None),
          images_(), descriptors_(), credits_(), bindings_(),
          reservedPages_(0), mappedPages_(0), reservationCount_(0),
          nextImageId_(1), nextPageSlot_(0)
    {
        if (maxImages_ == 0 || maxImages_ > kMaxImageCount ||
            maxChargedPages_ == 0 || maxChargedPages_ > kMaxChargedPages)
        {
            initializationError_ = Error::InvalidLimits;
            return;
        }

        // All writer storage is prepared once, before any image or page can be
        // committed.  Reserve/Grant/Encode therefore cannot throw or expose a
        // partially allocated reverse map.  The arrays stay at stable
        // addresses for the lifetime of the process codec.
        if (!ShouldFailAllocation(allocationFailureOrdinal))
            images_.reset(new (std::nothrow) ImageRecord[kMaxImageCount]);
        if (!ShouldFailAllocation(allocationFailureOrdinal))
            descriptors_.reset(new (std::nothrow) PageDescriptor[kUsablePageCount]);
        if (!ShouldFailAllocation(allocationFailureOrdinal))
            credits_.reset(new (std::nothrow) PageCredit[kUsablePageCount]);
        if (!ShouldFailAllocation(allocationFailureOrdinal))
            bindings_.reset(new (std::nothrow) BindingEntry[kBindingTableSize]);
        if (!images_ || !descriptors_ || !credits_ || !bindings_)
        {
            initializationError_ = Error::OutOfMemory;
            return;
        }

        for (uint32_t i = 0; i < kUsablePageCount; ++i)
        {
            descriptors_[i].rawPageBase.store(0, std::memory_order_relaxed);
            descriptors_[i].record.store(static_cast<ImageRecord*>(0),
                std::memory_order_relaxed);
            descriptors_[i].bound.store(0, std::memory_order_relaxed);
            descriptors_[i].admittedAtSeal = 0;
            credits_[i].next = kNoSlot;
            credits_[i].bound = 0;
        }
        for (uint32_t i = 0; i < kBindingTableSize; ++i)
        {
            bindings_[i].imageId = 0;
            bindings_[i].rawPageBase = 0;
            bindings_[i].slot = kNoSlot;
        }
        valid_ = true;
    }

    InterpreterMetadataIndexCodec(const InterpreterMetadataIndexCodec&) = delete;
    InterpreterMetadataIndexCodec& operator=(const InterpreterMetadataIndexCodec&) = delete;

    bool IsValid() const { return valid_; }
    Error InitializationError() const { return initializationError_; }
    uint32_t MaxImages() const { return maxImages_; }
    uint32_t MaxChargedPages() const { return maxChargedPages_; }

    // The descriptor array, image records and credit nodes are all stable and
    // preallocated.  Decode only performs bounded atomic loads and arithmetic.
    bool DecodeIsLockFree() const
    {
        return valid_ && descriptors_[0].rawPageBase.is_lock_free() &&
            descriptors_[0].record.is_lock_free() &&
            descriptors_[0].bound.is_lock_free() &&
            images_[0].lifecycle.is_lock_free();
    }

    // Reserve a complete batch of image IDs and page credits.  IDs are
    // allocated internally, ordinary and Shadow owners share this ledger, and
    // a failed batch leaves every counter and record unchanged.
    Error ReserveBatch(const BatchRequest* requests, size_t count,
        Reservation* outputs)
    {
        if (!valid_)
            return initializationError_;
        if (requests == 0 || outputs == 0 || count == 0)
            return Error::InvalidArgument;
        if (count > maxImages_)
            return Error::ImageLimit;

        std::lock_guard<std::mutex> guard(mutex_);
        if (nextImageId_ > maxImages_ || count > size_t(maxImages_ - nextImageId_ + 1u))
            return Error::ImageLimit;

        uint64_t totalCredits = 0;
        for (size_t i = 0; i < count; ++i)
        {
            if (requests[i].owner == 0)
                return Error::OwnerRequired;
            if (requests[i].credits == 0)
                return Error::ZeroCredits;
            if (totalCredits > std::numeric_limits<uint64_t>::max() - requests[i].credits)
                return Error::PageLimit;
            totalCredits += requests[i].credits;
        }
        if (totalCredits > uint64_t(maxChargedPages_) - reservedPages_ ||
            totalCredits > uint64_t(kUsablePageCount) - nextPageSlot_)
            return Error::PageLimit;

        // Every check above is complete and all storage is already present.
        // The following commit section has no throwing operation.
        const uint32_t firstImageId = nextImageId_;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t imageId = nextImageId_++;
        ImageRecord& image = images_[imageId - 1u];
            image.Reset(imageId, requests[i].owner, requests[i].credits);
            AppendCreditsLocked(image, requests[i].credits);
            outputs[i] = Reservation(imageId, requests[i].owner);
            ++reservationCount_;
            reservedPages_ += requests[i].credits;
        }
        (void)firstImageId;
        return Error::None;
    }

    Error Reserve(uint64_t owner, uint32_t credits, Reservation& output)
    {
        const BatchRequest request = {owner, credits};
        return ReserveBatch(&request, 1, &output);
    }

    // Seal the complete per-image footprint before publication.  The future
    // low range is [0, lowEnd); already mapped sparse pages outside that range
    // are unioned into the same demand count.  Missing pages are charged in
    // one global-ledger commit, and every failure leaves the reservation
    // unchanged.
    Error Finalize(const Reservation& reservation, uint64_t caller,
        uint64_t lowEnd)
    {
        if (!valid_)
            return initializationError_;
        if (lowEnd > kInterpreterRawDomainLimit)
            return Error::InvalidFootprint;

        std::lock_guard<std::mutex> guard(mutex_);
        ImageRecord* image = FindImageLocked(reservation.imageId);
        Error validation = ValidateHandleLocked(image, reservation);
        if (validation != Error::None)
            return validation;
        if (caller == 0)
            return Error::OwnerRequired;
        if (caller != image->owner)
            return Error::NotOwner;
        if (LoadLifecycle(*image) != Lifecycle::Private)
            return Error::InvalidState;
        if (image->sealed)
            return Error::AlreadyFinalized;

        const uint64_t lowPages = lowEnd == 0 ? 0 :
            (lowEnd + uint64_t(kPageSize) - 1u) / uint64_t(kPageSize);
        uint64_t mappedOutsideLow = 0;
        for (uint32_t slot = image->firstSlot; slot != kNoSlot;
            slot = credits_[slot].next)
        {
            if (credits_[slot].bound != 0 &&
                uint64_t(descriptors_[slot].rawPageBase.load(std::memory_order_relaxed)) >= lowEnd)
                ++mappedOutsideLow;
        }
        const uint64_t requiredPages = lowPages + mappedOutsideLow;
        if (requiredPages < lowPages ||
            requiredPages > uint64_t(maxChargedPages_) ||
            requiredPages > uint64_t(kUsablePageCount))
            return Error::PageLimit;
        const uint64_t additional = requiredPages > image->quota
            ? requiredPages - image->quota : 0;
        if (additional > uint64_t(maxChargedPages_) - reservedPages_ ||
            additional > uint64_t(kUsablePageCount) - nextPageSlot_ ||
            additional > std::numeric_limits<uint32_t>::max() - image->quota)
            return Error::PageLimit;

        // All arithmetic, ownership, and capacity checks are complete.  The
        // preallocated credit list makes this commit non-throwing.
        if (additional != 0)
        {
            AppendCreditsLocked(*image, static_cast<uint32_t>(additional));
            image->quota += static_cast<uint32_t>(additional);
            reservedPages_ += additional;
        }
        // This is the only point where preseal sparse-page membership becomes
        // immutable footprint admission.  A later lazy low-page binding does
        // not acquire this bit and must obey the exact raw lowEnd on every
        // subsequent Encode.
        for (uint32_t slot = image->firstSlot; slot != kNoSlot;
            slot = credits_[slot].next)
        {
            if (credits_[slot].bound != 0)
                descriptors_[slot].admittedAtSeal = 1;
        }
        image->lowEnd = lowEnd;
        image->requiredPages = requiredPages;
        image->sealed = true;
        return Error::None;
    }

    Error GetFootprintStats(const Reservation& reservation,
        FootprintStats& output) const
    {
        if (!valid_)
            return initializationError_;
        std::lock_guard<std::mutex> guard(mutex_);
        const ImageRecord* image = FindImageLocked(reservation.imageId);
        Error validation = ValidateHandleLocked(image, reservation);
        if (validation != Error::None)
            return validation;
        output.lowEnd = image->lowEnd;
        output.requiredPages = image->requiredPages;
        output.chargedPages = image->quota;
        output.mappedPages = image->mapped;
        output.sealed = image->sealed;
        return Error::None;
    }

    // Growth is a complete credit grant.  The global charge and per-image
    // noncontiguous slot list are committed before a later Encode can bind a
    // page.  Credits are never returned by Abort.
    Error GrantPages(const Reservation& reservation, uint64_t caller,
        uint32_t credits)
    {
        if (!valid_)
            return initializationError_;
        if (credits == 0)
            return Error::ZeroCredits;

        std::lock_guard<std::mutex> guard(mutex_);
        ImageRecord* image = FindImageLocked(reservation.imageId);
        Error validation = ValidateHandleLocked(image, reservation);
        if (validation != Error::None)
            return validation;
        if (caller == 0 || caller != image->owner)
            return caller == 0 ? Error::OwnerRequired : Error::NotOwner;
        const Lifecycle lifecycle = LoadLifecycle(*image);
        if (lifecycle == Lifecycle::Aborted)
            return Error::InvalidState;
        if (lifecycle != Lifecycle::Private && lifecycle != Lifecycle::Published)
            return Error::InvalidState;
        if (image->sealed)
            return Error::InvalidState;
        if (uint64_t(credits) > uint64_t(maxChargedPages_) - reservedPages_ ||
            uint64_t(credits) > uint64_t(kUsablePageCount) - nextPageSlot_ ||
            credits > std::numeric_limits<uint32_t>::max() - image->quota)
            return Error::PageLimit;

        image->quota += credits;
        AppendCreditsLocked(*image, credits);
        reservedPages_ += credits;
        return Error::None;
    }

    // Bind a raw page lazily.  A published page can be encoded by any caller;
    // a private page requires its reservation owner.  No reverse-map allocation
    // occurs here because the table was prepared by the constructor.
    Error Encode(const Reservation& reservation, int64_t rawIndex,
        uint64_t caller, int32_t& output) 
    {
        if (!valid_)
            return initializationError_;
        if (rawIndex == -1)
            return Error::Sentinel;
        if (rawIndex < 0 || uint64_t(rawIndex) >= kInterpreterRawDomainLimit)
            return Error::RawOutOfRange;

        const uint32_t raw = static_cast<uint32_t>(rawIndex);
        const uint32_t rawPageBase = raw & ~(kPageSize - 1u);
        const uint32_t offset = raw & (kPageSize - 1u);
        std::lock_guard<std::mutex> guard(mutex_);
        ImageRecord* image = FindImageLocked(reservation.imageId);
        Error validation = ValidateHandleLocked(image, reservation);
        if (validation != Error::None)
            return validation;
        if (!CanUseLocked(*image, caller))
            return caller == 0 ? Error::OwnerRequired : Error::NotOwner;

        const BindingEntry* existing = FindBindingLocked(image->imageId, rawPageBase);
        if (existing != 0)
        {
            const PageDescriptor& descriptor = descriptors_[existing->slot];
            if (image->sealed && descriptor.admittedAtSeal == 0 &&
                uint64_t(rawIndex) >= image->lowEnd)
                return Error::FootprintViolation;
            output = MakeToken(existing->slot, offset);
            return Error::None;
        }

        if (image->sealed && uint64_t(rawIndex) >= image->lowEnd)
            return Error::FootprintViolation;

        PageCredit* credit = FindNextUnboundCreditLocked(*image);
        if (credit == 0)
            return Error::QuotaExceeded;
        BindingEntry* empty = FindEmptyBindingLocked(image->imageId, rawPageBase);
        if (empty == 0)
            return Error::BindingMapFull;

        const uint32_t slot = static_cast<uint32_t>(credit - credits_.get());
        // The map, credit and descriptor are only observed by Decode after the
        // writer mutex is released.  Thus this sequence cannot expose a
        // partially visible binding, and it contains no operation that throws.
        empty->imageId = image->imageId;
        empty->rawPageBase = rawPageBase;
        empty->slot = slot;
        credit->bound = 1;
        image->nextUnboundSlot = credit->next;
        ++image->mapped;
        ++mappedPages_;
        PageDescriptor& descriptor = descriptors_[slot];
        descriptor.rawPageBase.store(rawPageBase, std::memory_order_relaxed);
        descriptor.record.store(image, std::memory_order_release);
        descriptor.bound.store(1, std::memory_order_release);
        output = MakeToken(slot, offset);
        return Error::None;
    }

    // Decode is intentionally const, lock-free, and allocation-free.  The
    // caller argument is only a private visibility check; published images are
    // accessible to unscoped and foreign callers.
    Error Decode(int32_t token, uint64_t caller, DecodedData& output) const
    {
        if (!valid_)
            return initializationError_;
        uint32_t slot = 0;
        uint32_t offset = 0;
        Error tokenError = DecodeToken(token, slot, offset);
        if (tokenError != Error::None)
            return tokenError;

        const PageDescriptor& descriptor = descriptors_[slot];
        if (descriptor.bound.load(std::memory_order_acquire) == 0)
            return Error::UnknownToken;
        ImageRecord* image = descriptor.record.load(std::memory_order_acquire);
        if (image == 0 || image->imageId == 0)
            return Error::UnknownToken;
        const Lifecycle lifecycle = LoadLifecycle(*image);
        if (lifecycle == Lifecycle::Aborted)
            return Error::InvalidState;
        if (lifecycle == Lifecycle::Private &&
            (caller == 0 || caller != image->owner))
            return caller == 0 ? Error::OwnerRequired : Error::NotOwner;

        const uint32_t rawPageBase = descriptor.rawPageBase.load(std::memory_order_relaxed);
        const uint64_t raw = uint64_t(rawPageBase) + offset;
        if (raw >= kInterpreterRawDomainLimit)
            return Error::RawOutOfRange;
        output.imageId = image->imageId;
        output.rawIndex = static_cast<int64_t>(raw);
        return Error::None;
    }

    // Return only the stable image identity carried by a bound token.  This
    // metadata-only lookup intentionally has no caller argument: adapters use
    // the ID to select their exact construction/staging scope, then call
    // Decode to enforce lifecycle and private-owner visibility.  It performs
    // no allocation or locking and leaves output unchanged on every error.
    // Aborted bindings report InvalidState, matching Decode's lifecycle rule.
    Error GetTokenImageId(int32_t token, uint32_t& imageId) const
    {
        if (!valid_)
            return initializationError_;
        uint32_t slot = 0;
        uint32_t ignoredOffset = 0;
        Error tokenError = DecodeToken(token, slot, ignoredOffset);
        if (tokenError != Error::None)
            return tokenError;

        const PageDescriptor& descriptor = descriptors_[slot];
        if (descriptor.bound.load(std::memory_order_acquire) == 0)
            return Error::UnknownToken;
        ImageRecord* image = descriptor.record.load(std::memory_order_acquire);
        if (image == 0 || image->imageId == 0)
            return Error::UnknownToken;
        if (LoadLifecycle(*image) == Lifecycle::Aborted)
            return Error::InvalidState;
        imageId = image->imageId;
        return Error::None;
    }

    Error TryAddOffset(const int32_t token, int64_t delta, uint64_t caller,
        int32_t& output) const
    {
        DecodedData decoded;
        Error error = Decode(token, caller, decoded);
        if (error != Error::None)
            return error;

        const int64_t base = decoded.rawIndex;
        if ((delta > 0 && base > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && base < std::numeric_limits<int64_t>::min() - delta))
            return Error::ArithmeticOverflow;
        const int64_t next = base + delta;
        if (next < 0 || uint64_t(next) >= kInterpreterRawDomainLimit)
            return Error::RawOutOfRange;

        // const is intentional for the read side, but binding a new lazy page
        // is a writer operation.  Re-enter the non-const method only after all
        // arithmetic and ownership checks have succeeded.
        return const_cast<InterpreterMetadataIndexCodec*>(this)->Encode(
            Reservation(decoded.imageId, FindOwnerForPublishedDecode(decoded.imageId, caller)),
            next, caller, output);
    }

    Error RawDifference(const int32_t left, const int32_t right,
        uint64_t caller, int64_t& output) const
    {
        DecodedData leftDecoded;
        DecodedData rightDecoded;
        Error error = Decode(left, caller, leftDecoded);
        if (error != Error::None)
            return error;
        error = Decode(right, caller, rightDecoded);
        if (error != Error::None)
            return error;
        if (leftDecoded.imageId != rightDecoded.imageId)
            return Error::CrossImage;
        output = leftDecoded.rawIndex - rightDecoded.rawIndex;
        return Error::None;
    }

    Error PublishBatch(const Reservation* reservations, size_t count)
    {
        if (!valid_)
            return initializationError_;
        if (reservations == 0 || count == 0)
            return Error::InvalidArgument;
        std::lock_guard<std::mutex> guard(mutex_);
        for (size_t index = 0; index < count; ++index)
        {
            const Reservation& reservation = reservations[index];
            if (reservation.owner == 0)
                return Error::OwnerRequired;
            ImageRecord* image = FindImageLocked(reservation.imageId);
            Error validation = ValidateHandleLocked(image, reservation);
            if (validation != Error::None)
                return validation;
            if (!image->sealed)
                return Error::Unfinalized;
            if (LoadLifecycle(*image) != Lifecycle::Private)
                return Error::InvalidState;
            for (size_t prior = 0; prior < index; ++prior)
                if (reservations[prior].imageId == reservation.imageId)
                    return Error::InvalidArgument;
        }
        // Validation is complete before the first transition. Runtime-level
        // publication gates the whole batch, so lock-free readers cannot
        // observe a partially published closure while these stores run.
        for (size_t index = 0; index < count; ++index)
        {
            ImageRecord* image = FindImageLocked(reservations[index].imageId);
            image->lifecycle.store(static_cast<uint8_t>(Lifecycle::Published),
                std::memory_order_release);
        }
        return Error::None;
    }

    Error Publish(const Reservation& reservation)
    {
        return PublishBatch(&reservation, 1);
    }

    Error Abort(const Reservation& reservation)
    {
        if (!valid_)
            return initializationError_;
        if (reservation.owner == 0)
            return Error::OwnerRequired;
        std::lock_guard<std::mutex> guard(mutex_);
        ImageRecord* image = FindImageLocked(reservation.imageId);
        Error validation = ValidateHandleLocked(image, reservation);
        if (validation != Error::None)
            return validation;
        uint8_t expected = static_cast<uint8_t>(Lifecycle::Private);
        return image->lifecycle.compare_exchange_strong(expected,
            static_cast<uint8_t>(Lifecycle::Aborted),
            std::memory_order_acq_rel, std::memory_order_acquire)
            ? Error::None : Error::InvalidState;
    }

    Stats GetStats() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        Stats stats = {reservedPages_, mappedPages_, reservationCount_,
            nextImageId_, nextPageSlot_};
        return stats;
    }

private:
    enum class Lifecycle : uint8_t
    {
        Private = 0,
        Published = 1,
        Aborted = 2,
    };

    static constexpr uint32_t kNoSlot = std::numeric_limits<uint32_t>::max();
    static constexpr uint32_t kBindingTableSize = 1u << 20;

    struct ImageRecord
    {
        uint32_t imageId;
        uint64_t owner;
        uint32_t quota;
        uint32_t mapped;
        uint32_t firstSlot;
        uint32_t lastSlot;
        uint32_t nextUnboundSlot;
        uint64_t lowEnd;
        uint64_t requiredPages;
        bool sealed;
        std::atomic<uint8_t> lifecycle;

        ImageRecord()
            : imageId(0), owner(0), quota(0), mapped(0), firstSlot(kNoSlot),
              lastSlot(kNoSlot), nextUnboundSlot(kNoSlot), lowEnd(0),
              requiredPages(0), sealed(false),
              lifecycle(static_cast<uint8_t>(Lifecycle::Aborted)) {}

        void Reset(uint32_t newImageId, uint64_t newOwner, uint32_t newQuota)
        {
            imageId = newImageId;
            owner = newOwner;
            quota = newQuota;
            mapped = 0;
            firstSlot = kNoSlot;
            lastSlot = kNoSlot;
            nextUnboundSlot = kNoSlot;
            lowEnd = 0;
            requiredPages = 0;
            sealed = false;
            lifecycle.store(static_cast<uint8_t>(Lifecycle::Private),
                std::memory_order_relaxed);
        }
    };

    struct PageDescriptor
    {
        std::atomic<uint32_t> rawPageBase;
        std::atomic<ImageRecord*> record;
        std::atomic<uint8_t> bound;
        uint8_t admittedAtSeal;
    };

    struct PageCredit
    {
        uint32_t next;
        uint8_t bound;
    };

    struct BindingEntry
    {
        uint32_t imageId;
        uint32_t rawPageBase;
        uint32_t slot;
    };

    const uint32_t maxImages_;
    const uint32_t maxChargedPages_;
    bool valid_;
    Error initializationError_;
    std::unique_ptr<ImageRecord[]> images_;
    std::unique_ptr<PageDescriptor[]> descriptors_;
    std::unique_ptr<PageCredit[]> credits_;
    std::unique_ptr<BindingEntry[]> bindings_;
    mutable std::mutex mutex_;
    uint64_t reservedPages_;
    uint64_t mappedPages_;
    uint32_t reservationCount_;
    uint32_t nextImageId_;
    uint32_t nextPageSlot_;

    static uint64_t HashKey(uint32_t imageId, uint32_t rawPageBase)
    {
        uint64_t value = (uint64_t(imageId) << 32) | rawPageBase;
        value ^= value >> 33;
        value *= UINT64_C(0xff51afd7ed558ccd);
        value ^= value >> 33;
        value *= UINT64_C(0xc4ceb9fe1a85ec53);
        value ^= value >> 33;
        return value;
    }

    static Lifecycle LoadLifecycle(const ImageRecord& image)
    {
        return static_cast<Lifecycle>(image.lifecycle.load(std::memory_order_acquire));
    }

    static bool ShouldFailAllocation(int32_t& ordinal)
    {
        if (ordinal < 0)
            return false;
        if (ordinal == 0)
            return true;
        --ordinal;
        return false;
    }

    static int32_t MakeToken(uint32_t slot, uint32_t offset)
    {
        const uint32_t encoded = kEncodedBase + slot * kPageSize + offset;
        return static_cast<int32_t>(encoded);
    }

    static Error DecodeToken(int32_t token, uint32_t& slot, uint32_t& offset)
    {
        if (token >= 0)
            return Error::AotDomain;
        if (token == -1)
            return Error::Sentinel;
        const uint32_t value = static_cast<uint32_t>(token);
        if (value < kEncodedBase || value > kEncodedLast)
            return Error::UnknownToken;
        const uint32_t pagePart = value - kEncodedBase;
        slot = pagePart >> kPageShift;
        offset = pagePart & (kPageSize - 1u);
        if (slot >= kUsablePageCount)
            return Error::UnknownToken;
        return Error::None;
    }

    ImageRecord* FindImageLocked(uint32_t imageId) const
    {
        if (imageId == 0 || imageId > maxImages_ || images_ == 0)
            return 0;
        ImageRecord* image = const_cast<ImageRecord*>(&images_[imageId - 1u]);
        return image->imageId == imageId ? image : 0;
    }

    static Error ValidateHandleLocked(const ImageRecord* image,
        const Reservation& reservation)
    {
        if (image == 0)
            return Error::InvalidImage;
        if (reservation.owner == 0)
            return Error::OwnerRequired;
        if (image->owner != reservation.owner)
            return Error::InvalidReservation;
        return Error::None;
    }

    static bool CanUseLocked(const ImageRecord& image, uint64_t caller)
    {
        const Lifecycle state = LoadLifecycle(image);
        return state != Lifecycle::Aborted &&
            (state == Lifecycle::Published ||
                (caller != 0 && caller == image.owner));
    }

    void AppendCreditsLocked(ImageRecord& image, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t slot = nextPageSlot_++;
            PageCredit& credit = credits_[slot];
            credit.next = kNoSlot;
            credit.bound = 0;
            if (image.lastSlot == kNoSlot)
                image.firstSlot = slot;
            else
                credits_[image.lastSlot].next = slot;
            image.lastSlot = slot;
            if (image.nextUnboundSlot == kNoSlot)
                image.nextUnboundSlot = slot;
        }
    }

    PageCredit* FindNextUnboundCreditLocked(ImageRecord& image)
    {
        uint32_t slot = image.nextUnboundSlot;
        while (slot != kNoSlot)
        {
            PageCredit& credit = credits_[slot];
            if (credit.bound == 0)
                return &credit;
            slot = credit.next;
        }
        image.nextUnboundSlot = kNoSlot;
        return 0;
    }

    const BindingEntry* FindBindingLocked(uint32_t imageId,
        uint32_t rawPageBase) const
    {
        const uint64_t hash = HashKey(imageId, rawPageBase);
        for (uint32_t probe = 0; probe < kBindingTableSize; ++probe)
        {
            const BindingEntry& entry = bindings_[(hash + probe) & (kBindingTableSize - 1u)];
            if (entry.imageId == 0)
                return 0;
            if (entry.imageId == imageId && entry.rawPageBase == rawPageBase)
                return &entry;
        }
        return 0;
    }

    BindingEntry* FindEmptyBindingLocked(uint32_t imageId,
        uint32_t rawPageBase)
    {
        const uint64_t hash = HashKey(imageId, rawPageBase);
        for (uint32_t probe = 0; probe < kBindingTableSize; ++probe)
        {
            BindingEntry& entry = bindings_[(hash + probe) & (kBindingTableSize - 1u)];
            if (entry.imageId == 0)
                return &entry;
            if (entry.imageId == imageId && entry.rawPageBase == rawPageBase)
                return &entry;
        }
        return 0;
    }

    // Published lookup is intentionally separate from caller ownership.  It
    // is used only to reconstruct the internal handle for owner-preserving
    // lazy arithmetic; private Decode has already proved the caller owner.
    uint64_t FindOwnerForPublishedDecode(uint32_t imageId, uint64_t caller) const
    {
        if (imageId == 0 || imageId > maxImages_ || images_ == 0)
            return caller;
        const ImageRecord& image = images_[imageId - 1u];
        return LoadLifecycle(image) == Lifecycle::Published ? image.owner : caller;
    }
};

} // namespace metadata
} // namespace hybridclr
