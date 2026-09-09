#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace hybridclr
{
namespace metadata
{
    struct InterpreterTypeCacheInsertion
    {
        // The caller holds the metadata lock. Encoding and map hashing must
        // not invoke metadata callbacks. All fallible preparation precedes
        // the final pointer append, so a retry cannot leave an orphan entry
        // in the image's type vector.
        template<typename Pointer, typename Map, typename Encoder>
        static uint32_t GetOrInsert(std::vector<Pointer>& types, Map& indices,
            Pointer type, const Encoder& encode)
        {
            static_assert(std::is_pointer<Pointer>::value, "type cache stores pointers");
            const auto found = indices.find(type);
            if (found != indices.end())
                return found->second;
            if (types.size() >= std::numeric_limits<uint32_t>::max() ||
                types.size() == types.max_size())
                throw std::length_error("interpreter type index range exhausted");

            if (types.size() == types.capacity())
            {
                const std::size_t extra = types.capacity() / 2 + 1;
                const std::size_t available = types.max_size() - types.capacity();
                types.reserve(types.capacity() + (extra < available ? extra : available));
            }
            const uint32_t encoded = encode(static_cast<uint32_t>(types.size()));
            const auto inserted = indices.insert(std::make_pair(type, encoded));
            if (!inserted.second)
                return inserted.first->second;
            // Capacity is already available and copying a pointer cannot
            // throw. No allocator or metadata callback runs after insertion.
            types.push_back(type);
            return encoded;
        }
    };
}
}
