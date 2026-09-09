#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "InterpreterMetadataIndexCodec.h"

namespace hybridclr { namespace metadata {
class InterpreterImage;

// Integration adapter; callers retain the ordinary loader / Shadow transaction
// publication protocol. The adapter never executes metadata callbacks.
class InterpreterMetadataIndexRuntime
{
public:
    using Codec = InterpreterMetadataIndexCodec;
    using Error = Codec::Error;
    using Reservation = Codec::Reservation;

    static Error Initialize();
    static Error ReserveImages(uint32_t count, std::vector<Reservation>& output);
    static Error Encode(uint32_t imageId, int64_t rawIndex, int32_t& output);
    static Error Decode(int32_t token, Codec::DecodedData& output);
    static Error Finalize(uint32_t imageId, uint64_t lowEnd);
    static Error Publish(uint32_t imageId);
    static Error PublishBatch(const uint32_t* imageIds, std::size_t count);
    static Error Abort(const Reservation& reservation);
    static Error Abort(uint32_t imageId);
    static Error GetStats(Codec::Stats& output);
    static Error GetFootprint(uint32_t imageId, Codec::FootprintStats& output);
    static InterpreterImage* GetPublishedImage(uint32_t imageId);
    static InterpreterImage* GetConstructionImage(uint32_t imageId);

    class ScopedConstruction
    {
    public:
        ScopedConstruction(const Reservation& reservation, InterpreterImage* image);
        ScopedConstruction(uint32_t imageId, InterpreterImage* image, bool shadow);
        ~ScopedConstruction();
        ScopedConstruction(const ScopedConstruction&) = delete;
        ScopedConstruction& operator=(const ScopedConstruction&) = delete;
    private:
        friend class InterpreterMetadataIndexRuntime;
        uint32_t imageId_;
        InterpreterImage* image_;
        ScopedConstruction* previous_;
    };

private:
    static uint64_t CurrentOwner(uint32_t imageId);
    static thread_local ScopedConstruction* construction_;
};
}}
