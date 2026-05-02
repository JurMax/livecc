#pragma once

#define M_EXPAND(x) x
#define M_EXPAND_VA(...) __VA_ARGS__

#define __M_GLUE(x, y) x##y
#define M_GLUE(x, y) __M_GLUE(x, y)

#define __M_STR(x) #x
#define M_STR(x) __M_STR(x)

#define M_VARG_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,_21,_22,_23,_24,_25,_26,_27,_28,_29,_30,_31,_32,N,...) N
#define M_VARG_COUNT(...) M_EXPAND(M_VARG_N(__VA_ARGS__,32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0))

#define M_STR_VARG(...) M_EXPAND(M_GLUE(M_STR_, M_VARG_COUNT(__VA_ARGS__))(__VA_ARGS__))
#define M_STR_1( x )     M_STR(x)
#define M_STR_2( x, ...) M_STR(x) ", " M_STR_1( __VA_ARGS__)
#define M_STR_3( x, ...) M_STR(x) ", " M_STR_2( __VA_ARGS__)
#define M_STR_4( x, ...) M_STR(x) ", " M_STR_3( __VA_ARGS__)
#define M_STR_5( x, ...) M_STR(x) ", " M_STR_4( __VA_ARGS__)
#define M_STR_6( x, ...) M_STR(x) ", " M_STR_5( __VA_ARGS__)
#define M_STR_7( x, ...) M_STR(x) ", " M_STR_6( __VA_ARGS__)
#define M_STR_8( x, ...) M_STR(x) ", " M_STR_7( __VA_ARGS__)
#define M_STR_9( x, ...) M_STR(x) ", " M_STR_8( __VA_ARGS__)
#define M_STR_10(x, ...) M_STR(x) ", " M_STR_9( __VA_ARGS__)
#define M_STR_11(x, ...) M_STR(x) ", " M_STR_10(__VA_ARGS__)
#define M_STR_12(x, ...) M_STR(x) ", " M_STR_11(__VA_ARGS__)
#define M_STR_13(x, ...) M_STR(x) ", " M_STR_12(__VA_ARGS__)
#define M_STR_14(x, ...) M_STR(x) ", " M_STR_13(__VA_ARGS__)
#define M_STR_15(x, ...) M_STR(x) ", " M_STR_14(__VA_ARGS__)
#define M_STR_16(x, ...) M_STR(x) ", " M_STR_15(__VA_ARGS__)

#ifdef NDEBUG
    #define no_inline
#else
    #if defined(__clang__)
        #define no_inline [[clang::noinline]]
    #elif defined(_MSC_VER)
        #define no_inline [[msvc::noinline]]
    #else
        #define no_inline [[gnu::noinline]]
    #endif
#endif

typedef unsigned int uint;
typedef unsigned short ushort;

namespace livecc {
    struct None {};
    struct Empty {};
    struct Copy {};
    struct Unsafe {};

    enum class ErrorCode {
        OK,
        FAILED,

        OPEN_FAILED,
        UNEXPECTED_END,
        BUFFER_TOO_SMALL,

        NO_INPUT,

        EXISTS_ALREADY
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

    struct NoMove {
        constexpr inline NoMove() {}
        constexpr inline NoMove( decltype(nullptr) ) {}
        NoMove( NoMove const& ) = delete;
        NoMove( NoMove&& ) = delete;
    };
}
