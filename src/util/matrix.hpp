
#include "log.hpp"
#include "math.hpp"
#include "option.hpp"

namespace livecc {

    struct mat3 {
        // List of rows.
        float arr[3][3];

        constexpr inline decltype(auto) operator()( this auto&& self, uint row, uint col ) {
            return self.arr[row][col];
        }

        constexpr inline Option<mat3> invert( this mat3 const& m ) {
            float det = m(0, 0) * (m(1, 1) * m(2, 2) - m(2, 1) * m(1, 2)) -
                        m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
                        m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
            if (math::close_to(det, 0.f))
                return {};
            float inv_det = 1 / det;
            mat3 inv;
            inv(0, 0) = (m(1, 1) * m(2, 2) - m(2, 1) * m(1, 2)) * inv_det;
            inv(0, 1) = (m(0, 2) * m(2, 1) - m(0, 1) * m(2, 2)) * inv_det;
            inv(0, 2) = (m(0, 1) * m(1, 2) - m(0, 2) * m(1, 1)) * inv_det;
            inv(1, 0) = (m(1, 2) * m(2, 0) - m(1, 0) * m(2, 2)) * inv_det;
            inv(1, 1) = (m(0, 0) * m(2, 2) - m(0, 2) * m(2, 0)) * inv_det;
            inv(1, 2) = (m(1, 0) * m(0, 2) - m(0, 0) * m(1, 2)) * inv_det;
            inv(2, 0) = (m(1, 0) * m(2, 1) - m(2, 0) * m(1, 1)) * inv_det;
            inv(2, 1) = (m(2, 0) * m(0, 1) - m(0, 0) * m(2, 1)) * inv_det;
            inv(2, 2) = (m(0, 0) * m(1, 1) - m(1, 0) * m(0, 1)) * inv_det;
            return {inv};
        }

        constexpr inline mat3 mul( this mat3 const& a, mat3 const& b ) {
            mat3 ret{{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}}};
            for (uint row = 0; row < 3; ++row)
                for (uint col = 0; col < 3; ++col)
                    for (uint i = 0; i < 3; ++i)
                        ret(row, col) += a(row, i) * b(i, col);
            return ret;
        }

        constexpr inline vec2 mul( this mat3 const& m, vec2 const& b ) {
            float in[3] = {b.x, b.y, 1.f};
            float out[3] = {0.f, 0.f, 0.f};
            for (uint row = 0; row < 3; ++row)
                for (uint col = 0; col < 3; ++col)
                    out[row] += m(row, col) * in[col];
            return {out[0] / out[2], out[1] / out[2]};
        }

        constexpr Span<float> span() { return {&arr[0][0], 9u}; }


        static constexpr inline mat3 identity() {
            return mat3{{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}}};
        }

        /**
         * map  {[1, -1], [1, 1], [-1, 1], [-1, -1]} to {p1, p2, p3, p4}.
         */
        static constexpr inline Option<mat3> perspective(vec2 p1, vec2 p2, vec2 p3, vec2 p4) {
            // if (math::close_to(p2.y - p3.y, 0.f) || math::close_to(p4.x - p3.x, 0.f))
            //     return {};
            // float j = (p1.y - p2.y + p3.y - p4.y) / (p2.y - p3.y);
            // float m = (p4.y - p3.y)               / (p2.y - p3.y);
            // float k = (p1.x - p2.x + p3.x - p4.x) / (p4.x - p3.x);
            // float n = (p2.x - p3.x)               / (p4.x - p3.x);

            // float diff = (1.f - m * n);
            // if (math::close_to(diff, 0.f))
            //     return {};
            // float h = (j - k * m) / diff;
            // float g = (k - j * n) / diff;

            // float c = p1.x;
            // float f = p1.y;
            // float a = p4.x * (g + 1.f) - p1.x;
            // float d = p4.y * (g + 1.f) - p1.y;
            // float b = p2.x * (h + 1.f) - p1.x;
            // float e = p2.y * (h + 1.f) - p1.y;

            float j =  p1.x - p2.x - p3.x + p4.x;
            float k = -p1.x - p2.x + p3.x + p4.x;
            float l = -p1.x + p2.x - p3.x + p4.x;
            float m =  p1.y - p2.y - p3.y + p4.y;
            float n = -p1.y - p2.y + p3.y + p4.y;
            float o = -p1.y + p2.y - p3.y + p4.y;
            float i = 1;
            float denom = (m * k - j * n);
            float h = math::close_to(denom, 0.f) ? 0.f : (j * o - m * l) * i / denom;
            float g = math::close_to(j, 0.f) ? 0.f : (k * h + l * i) / j;
            float f = (p1.y * (g + h + i) + p3.y * (-g - h + i)) / 2;
            float e = (p1.y * (g + h + i) - p2.y * ( g - h + i)) / 2;
            float d =  p1.y * (g + h + i) - f - e;
            float c = (p1.x * (g + h + i) + p3.x * (-g - h + i)) / 2;
            float b = (p1.x * (g + h + i) - p2.x * ( g - h + i)) / 2;
            float a =  p1.x * (g + h + i) - c - b;
            return {mat3{{{a, b, c}, {d, e, f}, {g, h, i}}}};
        }

        /** (1,0), (0,1), (0,0) */
        static constexpr inline Option<mat3> affine(vec2 p1, vec2 p2, vec2 p3) {
            return {mat3{{
                {p1.x - p3.x, p2.x - p3.x, p3.x},
                {p1.y - p3.y, p2.y - p3.y, p3.y},
                {0.f, 0.f, 1.f}}}};
        }



        static constexpr inline Option<mat3> transform_points(
                vec2 a1, vec2 a2, vec2 a3, vec2 a4,
                vec2 b1, vec2 b2, vec2 b3, vec2 b4 ) {
            if_let (A, perspective(a1, a2, a3, a4))
            if_let (A_inv, A.invert())
            if_let (B, perspective(b1, b2, b3, b4))
                return B.mul(A_inv);
            return {};
        }
    };
}
