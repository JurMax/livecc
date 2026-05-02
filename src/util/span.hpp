#pragma once

#include "option.hpp"
#include "math.hpp"

#include <format>
#include <string_view>

namespace livecc {
    struct EndIterator{};

    template<typename T>
    struct Span {
        T* ptr;
        uint len;
        uint capacity;

        constexpr inline Span() :
            ptr(nullptr), len(0), capacity(0) {}
        constexpr inline Span( T* data, uint length ) :
            ptr(data), len(length), capacity(length) {}
        constexpr inline Span( T* data, uint length, uint capacity ) :
            ptr(data), len(length), capacity(capacity) {}
        template<uint L>
        constexpr inline Span( T (&arr)[L] ) :
            ptr(arr), len(L - (uint)std::is_same_v<T, char const>),
            capacity(L) {}

        constexpr inline operator Span<const T>() const {
            return {ptr, len, capacity};
        }

        constexpr inline T* begin() { return ptr; }
        constexpr inline T* end() { return ptr + len; }
        constexpr inline T const* begin() const { return ptr; }
        constexpr inline T const* end() const { return ptr + len; }

        constexpr inline T& operator[]( uint i ) { return this->ptr[i]; }
        constexpr inline T const& operator[]( uint i ) const { return this->ptr[i]; }

        constexpr inline uint space() const { return capacity - len; }

        constexpr inline Option<T&> first( this Span self ) { if (self.len != 0u) return {self.ptr[0u]}; else return {}; }
        constexpr inline Option<T&>  last( this Span self ) { if (self.len != 0u) return {self.ptr[self.len - 1u]}; else return {}; }

        template<typename F> requires requires (F f, T t) { f(t); }
        constexpr inline Option<T&> first( this Span self, F&& func, uint start = 0 ) {
            for (uint i = start; i < self.len; ++i)
                if (func(self.ptr[i]))
                    return {self.ptr[i]};
            return {};
        }


        constexpr inline Option<uint> find( this Span self, T c, uint start = 0 ) {
            for (uint i = start; i < self.len; ++i)
                if (self.ptr[i] == c)
                    return {i};
            return {};
        }

        constexpr inline Option<uint> find( this Span self, Span<T const> other, uint start = 0 ) {
            if (other.len == 0u) return false;
            for (uint j = 0u, i = start; i < self.len; ++i) {
                if (self.ptr[i] == other.ptr[j]) {
                    ++j;
                    if (j == other.len)
                        return {i - (j - 1u)};
                }
                else
                    j = 0u;
            }
            return {};
        }

        template<typename F> requires requires (F f, T t) { f(t); }
        constexpr inline Option<uint> find( this Span self, F&& func, uint start = 0 ) {
            for (uint i = start; i < self.len; ++i)
                if (func(self.ptr[i]))
                    return {i};
            return {};
        }

        constexpr inline Option<uint> find_last( this Span self, T c ) {
            for (uint i = self.len; i-- > 0u;)
                if (self.ptr[i] == c)
                    return {i};
            return {};
        }

        /** Get the index of the end of the current line. */
        constexpr inline uint find_end_of_line( this Span self, uint start = 0 ) {
            for (uint i = start; i < self.len; ++i)
                if (self.ptr[i] == '\n')
                    return i;
            return self.len;
        }
        /** Get the index of the beginning of the next line. */
        constexpr inline uint find_next_line( this Span self, uint start = 0 ) {
            for (uint i = start; i < self.len; ++i)
                if (self.ptr[i] == '\n')
                    return i + 1;
            return self.len;
        }

        constexpr inline bool contains( this Span self, T chr, uint start = 0 ) { return self.find(chr, start).has_value(); }
        constexpr inline bool contains( this Span self, Span<T const> other, uint start = 0 ) { return self.find(other, start).has_value(); }
        template<typename F> requires requires (F f, T t) { f(t); }
        constexpr inline bool contains( this Span self, F&& func, uint start = 0 ) { return self.find<F>(std::forward<F>(func), start).has_value(); }

        template<typename F> requires requires (F f, T t) { (bool)f(t); }
        constexpr inline uint count( this Span self, F&& func, uint start = 0 ) {
            uint total = 0;
            for (uint i = start; i < self.len; ++i)
                if (func(self.ptr[i]))
                    total++;
            return total;
        }

        constexpr inline bool starts_with( this Span self, T const& other ) {
            return self.len == 0 ? false : self.ptr[0] == other;
        }
        constexpr inline bool ends_with( this Span self, T const& other ) {
            return self.len == 0 ? false : self.ptr[self.len - 1] == other;
        }

        constexpr inline bool starts_with( this Span self, Span<T const> other, uint skip = 0 ) {
            if (skip + other.len > self.len)
                return false;
            for (uint i = 0; i < other.len; ++i)
                if (self.ptr[skip + i] != other.ptr[i])
                    return false;
            return true;
        }
        constexpr inline bool ends_with( this Span self, Span<T const> other ) {
            return self.starts_with(other, self.len - other.len);
        }

        constexpr inline bool operator==( this Span self, Span<T const> other ) {
            if (self.len != other.len)
                return false;
            for (uint i = 0; i < other.len; ++i)
                if (self.ptr[i] != other.ptr[i])
                    return false;
            return true;
        }
        constexpr inline bool operator!=( this Span self, Span<T const> other ) {
            return !(self == other);
        }

        constexpr Span span( this Span self ) { return self; }
        constexpr Span slice( this Span self, uint start_i ) {
            math::mut_min(start_i, self.len);
            return {self.ptr + start_i, self.len - start_i};
        }
        constexpr Span slice( this Span self, uint start_i, uint end_i ) {
            math::mut_min(start_i, self.len);
            math::mut_min(end_i, self.len);
            return {self.ptr + start_i, end_i - start_i};
        }
        constexpr Span take( this Span self, uint new_length ) {
            math::mut_min(new_length, self.len);
            return {self.ptr, new_length};
        }

        constexpr Span trim_front( this Span self, T const& with ) {
            while (self.len > 0 && self.ptr[0u] == with) {
                self.ptr++;
                self.len--;
            }
            self.capacity = 0;
            return self;
        }
        constexpr Span trim_back( this Span self, T const& with ) {
            while (self.len > 0 && self.ptr[self.len - 1u] == with)
                self.len--;
            self.capacity = 0;
            return self;
        }
        constexpr Span trim( this Span self, T const& with ) {
            return self.trim_front(with).trim_back(with);
        }

        constexpr inline size_t hash() const {
            // Source: http://www.cse.yorku.ca/~oz/hash.html
            size_t hash = 5381;
            for (uint i = 0, l = this->len; i < l; ++i) /* hash * 33 + c */
                hash = ((hash << 5) + hash) + (size_t) this->ptr[i];
            return hash;
        }

        constexpr inline void unsafe_option_set() { capacity = (uint)-1; }
        constexpr inline bool unsafe_option_get() const { return capacity != (uint)-1; }

    protected:
        constexpr inline void swap( Span& other ) {
            T* temp_ptr = this->ptr;
            uint temp_len = this->len;
            uint temp_capacity = this->capacity;
            this->ptr = other.ptr;
            this->len = other.len;
            this->capacity = other.capacity;
            other.ptr = temp_ptr;
            other.len = temp_len;
            other.capacity = temp_capacity;
        }
    };


    template<typename T, uint L>
    struct Array {
        T arr[L];
        static constexpr inline uint len = L;

        constexpr inline decltype(auto) operator[]( this auto&& self, size_t i ) {
            return self.arr[i];
        }
        constexpr inline auto begin( this auto&& self ) { return self.arr + 0; }
        constexpr inline auto end( this auto&& self ) { return self.arr + L; }

        constexpr inline Span<T> span() { return {arr, L, L}; }
        constexpr inline Span<T const> span() const { return {arr, L, L}; }
    };
}

template<typename T>
struct std::formatter<livecc::Span<T>> {
    constexpr inline auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    inline auto format(livecc::Span<T> span, std::format_context& ctx) const {
        std::format_to(ctx.out(), "[");
        for (uint i = 0; i < span.len; ++i) {
            if (i != 0) std::format_to(ctx.out(), ", ");
            std::format_to(ctx.out(), "{}", span[i]);
        }
        return std::format_to(ctx.out(), "]");
    }
};



template<>
struct std::formatter<livecc::Span<char const>> : std::formatter<std::string_view> {
    auto format(livecc::Span<char const> const& span, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format({span.ptr, span.len}, ctx);
    }
};
