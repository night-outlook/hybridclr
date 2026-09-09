#include "InterpreterMetadataIndexRuntime.h"
#include "AssemblyShadowBridge.h"

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace hybridclr { namespace metadata {
namespace {
using Runtime = InterpreterMetadataIndexRuntime;
using Codec = Runtime::Codec;
struct Record
{
    Codec::Reservation reservation;
    std::atomic<InterpreterImage*> image;
    std::atomic<uint32_t> publicationGroup;
    std::atomic<uint8_t> shadow;
    Record() : image(nullptr), publicationGroup(0), shadow(0) {}
};
struct PublicationGroup
{
    std::atomic<uint8_t> active;
    PublicationGroup() : active(0) {}
};
struct RuntimeState
{
    Codec codec;
    Record records[Codec::kMaxImageCount + 1];
    PublicationGroup groups[Codec::kMaxImageCount + 1];
    Codec::Reservation publishScratch[Codec::kMaxImageCount];
    uint64_t nextOwner;
    uint32_t nextPublicationGroup;
    RuntimeState() : nextOwner(1), nextPublicationGroup(1) {}
};
std::atomic<RuntimeState*> state(nullptr);
std::mutex reservationMutex;

RuntimeState* Ready() { return state.load(std::memory_order_acquire); }
Record* Associated(RuntimeState* current, uint32_t id)
{
    if (!current || id == 0 || id > Codec::kMaxImageCount)
        return nullptr;
    Record& record = current->records[id];
    // Association release-publishes the stable reservation fields. No record
    // is reused or rebound to another image after this point.
    return record.image.load(std::memory_order_acquire) ? &record : nullptr;
}

bool Public(RuntimeState* current, Record* record)
{
    if (!current || !record)
        return false;
    const uint32_t group = record->publicationGroup.load(std::memory_order_acquire);
    if (group == 0 || group > Codec::kMaxImageCount ||
        current->groups[group].active.load(std::memory_order_acquire) == 0)
        return false;
    InterpreterImage* image = record->image.load(std::memory_order_acquire);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
    if (record->shadow.load(std::memory_order_acquire) != 0)
        return AssemblyShadowBridge::IsPublicImage(record->reservation.imageId, image);
#endif
    return image != nullptr;
}
}

thread_local Runtime::ScopedConstruction* Runtime::construction_ = nullptr;

Runtime::Error Runtime::Initialize()
{
    if (Ready()) return Error::None;
    std::lock_guard<std::mutex> lock(reservationMutex);
    if (Ready()) return Error::None;
    std::unique_ptr<RuntimeState> fresh(new (std::nothrow) RuntimeState());
    if (!fresh) return Error::OutOfMemory;
    if (!fresh->codec.IsValid()) return fresh->codec.InitializationError();
    state.store(fresh.release(), std::memory_order_release);
    return Error::None;
}

Runtime::Error Runtime::ReserveImages(uint32_t count, std::vector<Reservation>& output)
{
    if (count == 0) return Error::InvalidArgument;
    if (count > Codec::kMaxImageCount) return Error::ImageLimit;
    Error error = Initialize();
    if (error != Error::None) return error;
    std::vector<Codec::BatchRequest> requests(count);
    std::vector<Reservation> reserved(count);
    std::lock_guard<std::mutex> lock(reservationMutex);
    RuntimeState& current = *Ready();
    if (current.nextOwner == std::numeric_limits<uint64_t>::max())
        return Error::ArithmeticOverflow;
    const uint64_t owner = current.nextOwner;
    for (auto& request : requests) { request.owner = owner; request.credits = 1; }
    error = current.codec.ReserveBatch(requests.data(), requests.size(), reserved.data());
    if (error != Error::None) return error;
    // Only fixed stores and noexcept swap follow the kernel reservation.
    for (const Reservation& reservation : reserved)
        current.records[reservation.imageId].reservation = reservation;
    ++current.nextOwner;
    output.swap(reserved);
    return Error::None;
}

Runtime::ScopedConstruction::ScopedConstruction(const Reservation& reservation,
    InterpreterImage* image)
    : imageId_(reservation.imageId), image_(image), previous_(nullptr)
{
    std::lock_guard<std::mutex> lock(reservationMutex);
    RuntimeState* current = Ready();
    if (!current || !image || imageId_ == 0 || imageId_ > Codec::kMaxImageCount)
        throw std::invalid_argument("invalid interpreter construction scope");
    Record& record = current->records[imageId_];
    if (record.reservation.imageId != imageId_ ||
        record.reservation.owner != reservation.owner || reservation.owner == 0)
        throw std::invalid_argument("construction scope requires its exact image reservation");
    InterpreterImage* associated = record.image.load(std::memory_order_acquire);
    if (associated && associated != image)
        throw std::invalid_argument("interpreter image reservation cannot be rebound");
    record.image.store(image, std::memory_order_release);
    previous_ = construction_;
    construction_ = this;
}

Runtime::ScopedConstruction::ScopedConstruction(uint32_t imageId,
    InterpreterImage* image, bool shadow)
    : imageId_(imageId), image_(image), previous_(nullptr)
{
    std::lock_guard<std::mutex> lock(reservationMutex);
    RuntimeState* current = Ready();
    if (!current || !image || imageId_ == 0 || imageId_ > Codec::kMaxImageCount)
        throw std::invalid_argument("invalid interpreter construction scope");
    Record& record = current->records[imageId_];
    if (record.reservation.imageId != imageId_ || record.reservation.owner == 0)
        throw std::invalid_argument("construction scope requires a reserved image identity");
    InterpreterImage* associated = record.image.load(std::memory_order_acquire);
    if (associated && associated != image)
        throw std::invalid_argument("interpreter image reservation cannot be rebound");
    const uint8_t expectedShadow = shadow ? 1 : 0;
    if (associated && record.shadow.load(std::memory_order_acquire) != expectedShadow)
        throw std::invalid_argument("interpreter image reservation kind cannot change");
    record.shadow.store(expectedShadow, std::memory_order_relaxed);
    record.image.store(image, std::memory_order_release);
    previous_ = construction_;
    construction_ = this;
}

Runtime::ScopedConstruction::~ScopedConstruction()
{
    construction_ = previous_;
}

InterpreterImage* Runtime::GetConstructionImage(uint32_t imageId)
{
    for (ScopedConstruction* scope = construction_; scope; scope = scope->previous_)
        if (scope->imageId_ == imageId) return scope->image_;
    return nullptr;
}

uint64_t Runtime::CurrentOwner(uint32_t imageId)
{
    RuntimeState* current = Ready();
    Record* record = Associated(current, imageId);
    if (!record) return 0;
    InterpreterImage* candidate = GetConstructionImage(imageId);
#if HYBRIDCLR_ENABLE_ASSEMBLY_SHADOW
    if (!candidate) candidate = AssemblyShadowBridge::GetPrivateImage(imageId);
#endif
    if (!candidate && Public(current, record))
        candidate = record->image.load(std::memory_order_acquire);
    return candidate && candidate == record->image.load(std::memory_order_acquire)
        ? record->reservation.owner : 0;
}

Runtime::Error Runtime::Encode(uint32_t imageId, int64_t rawIndex, int32_t& output)
{
    RuntimeState* current = Ready();
    Record* record = Associated(current, imageId);
    if (!record) return Error::InvalidImage;
    const uint64_t owner = CurrentOwner(imageId);
    if (owner == 0) return Error::OwnerRequired;
    Error error = current->codec.Encode(record->reservation, rawIndex, owner, output);
    if (error != Error::QuotaExceeded) return error;
    Codec::FootprintStats footprint;
    error = current->codec.GetFootprintStats(record->reservation, footprint);
    if (error != Error::None) return error;
    if (footprint.sealed) return Error::QuotaExceeded;
    // Private eager initialization can grow its charged quota. After sealing,
    // all lazy mappings must use the permanently reserved final footprint.
    error = current->codec.GrantPages(record->reservation, owner, 1);
    return error == Error::None
        ? current->codec.Encode(record->reservation, rawIndex, owner, output) : error;
}

Runtime::Error Runtime::Decode(int32_t token, Codec::DecodedData& output)
{
    RuntimeState* current = Ready();
    if (!current) return Error::InvalidState;
    uint32_t imageId = 0;
    Error error = current->codec.GetTokenImageId(token, imageId);
    if (error != Error::None) return error;
    const uint64_t owner = CurrentOwner(imageId);
    return owner == 0 ? Error::OwnerRequired : current->codec.Decode(token, owner, output);
}

Runtime::Error Runtime::Finalize(uint32_t imageId, uint64_t lowEnd)
{
    RuntimeState* current = Ready();
    Record* record = Associated(current, imageId);
    return record ? current->codec.Finalize(record->reservation, CurrentOwner(imageId), lowEnd)
        : Error::InvalidImage;
}

Runtime::Error Runtime::Publish(uint32_t imageId)
{
    return PublishBatch(&imageId, 1);
}

Runtime::Error Runtime::PublishBatch(const uint32_t* imageIds, size_t count)
{
    if (!imageIds || count == 0 || count > Codec::kMaxImageCount)
        return Error::InvalidArgument;
    RuntimeState* current = Ready();
    if (!current) return Error::InvalidState;
    std::lock_guard<std::mutex> lock(reservationMutex);
    if (current->nextPublicationGroup > Codec::kMaxImageCount)
        return Error::InvalidState;
    const uint32_t group = current->nextPublicationGroup;
    for (size_t index = 0; index < count; ++index)
    {
        Record* record = Associated(current, imageIds[index]);
        if (!record || !record->image.load(std::memory_order_acquire))
            return Error::InvalidImage;
        if (record->publicationGroup.load(std::memory_order_acquire) != 0)
            return Error::InvalidState;
        current->publishScratch[index] = record->reservation;
    }
    Error error = current->codec.PublishBatch(current->publishScratch, count);
    if (error != Error::None) return error;
    for (size_t index = 0; index < count; ++index)
        current->records[imageIds[index]].publicationGroup.store(group, std::memory_order_release);
    current->groups[group].active.store(1, std::memory_order_release);
    ++current->nextPublicationGroup;
    return Error::None;
}

Runtime::Error Runtime::Abort(const Reservation& reservation)
{
    RuntimeState* current = Ready();
    return current ? current->codec.Abort(reservation) : Error::InvalidState;
}

Runtime::Error Runtime::Abort(uint32_t imageId)
{
    RuntimeState* current = Ready();
    if (!current) return Error::InvalidState;
    std::lock_guard<std::mutex> lock(reservationMutex);
    Record* record = Associated(current, imageId);
    if (!record)
    {
        if (imageId == 0 || imageId > Codec::kMaxImageCount ||
            current->records[imageId].reservation.imageId != imageId)
            return Error::InvalidImage;
        record = &current->records[imageId];
    }
    if (record->publicationGroup.load(std::memory_order_acquire) != 0)
        return Error::InvalidState;
    return current->codec.Abort(record->reservation);
}

InterpreterImage* Runtime::GetPublishedImage(uint32_t imageId)
{
    RuntimeState* current = Ready();
    Record* record = Associated(current, imageId);
    return Public(current, record)
        ? record->image.load(std::memory_order_acquire) : nullptr;
}

Runtime::Error Runtime::GetStats(Codec::Stats& output)
{
    RuntimeState* current = Ready();
    if (!current) return Error::InvalidState;
    output = current->codec.GetStats();
    return Error::None;
}

Runtime::Error Runtime::GetFootprint(uint32_t imageId, Codec::FootprintStats& output)
{
    RuntimeState* current = Ready();
    Record* record = Associated(current, imageId);
    return record ? current->codec.GetFootprintStats(record->reservation, output) : Error::InvalidImage;
}
}}
