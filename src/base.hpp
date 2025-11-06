#pragma once
#include <sys/types.h>

enum class ErrorCode {
    OK,
    FAILED,

    OPEN_FAILED,
    UNEXPECTED_END,
    BUFFER_TOO_SMALL
};

struct Range {
    struct Iterator {
        uint i;
        constexpr inline Iterator& operator++() { ++i; return *this; }
        constexpr inline bool operator!=( Iterator& o ) { return i != o.i; }
        constexpr inline uint operator*() { return i; }
    };
    constexpr inline Range( uint size ) : size(size) {}
    template<typename T> requires requires (T t) { (uint)t.size(); }
    constexpr inline Range( T iterable ) : size((uint)iterable.size()) {}

    constexpr inline Iterator begin() { return {0}; }
    constexpr inline Iterator end() { return {size}; }
    uint size;
};
