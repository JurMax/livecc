#include "memory.hpp"

#include <cstdlib>

#include "math.hpp"

livecc::Option<void*> livecc::allocate_data( uint length, uint alignment ) {
    uint actual_length = math::round_up(length, alignment);
    void* data = std::aligned_alloc(alignment, actual_length);
    if (data != nullptr)
        return {data};
    return {};
}

void livecc::deallocate_data( void* data ) {
    std::free(data);
}
