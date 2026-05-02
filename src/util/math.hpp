#pragma once

namespace livecc::math {
    static constexpr inline float PI = 3.14159265358979323846f;

    template<typename T>
    [[nodiscard]] constexpr inline T round_up( T num_to_round, T multiple ) {
        T remainder = num_to_round % multiple;
        if (remainder == 0)
            return num_to_round;
        return num_to_round + multiple - remainder;
    }

    template<typename T, typename ...Ts>
    [[nodiscard]] constexpr inline T min( T a, Ts... b ) {
        T smallest = a;
        ((b < smallest ? (smallest = b) : b), ...);
        return smallest;
    }
    template<typename T, typename ...Ts>
    [[nodiscard]] constexpr inline T max( T a, auto const&... b ) {
        T biggest = a;
        ((b < biggest ? b : (biggest = b)), ...);
        return biggest;
    }
    template<typename T>
    [[nodiscard]] constexpr inline T clamp( T value, T min, T max ) {
        if (value <= min) return min;
        if (value >= max) return max;
        return value;
    }

    constexpr inline auto& mut_min( auto& a, auto const&... b ) {
        return ((b < a ? (a = b) : b), ..., a);
    }
    constexpr inline auto& mut_max( auto& a, auto const&... b ) {
        return ((b < a ? b : (a = b)), ..., a);
    }

    constexpr inline float abs(float a) {
        return a < 0.f ? -a : a;
    }

    constexpr inline bool close_to( float a, float b ) {
        return math::abs(a - b) < 1e-10;
    }

    constexpr inline float distance( float a, float b ) {
        return math::abs(b - a);
    }

    constexpr inline float lerp( float a, float b, float t ) {
        return a * (1.f - t) + t * b;
    }
}
