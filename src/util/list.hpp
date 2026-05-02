#pragma once

#include "span.hpp"
#include "math.hpp"
#include "memory.hpp"

namespace livecc {

    /**
     * A growable data array.
     * NOTE: moves data by copying the bytes, so use this only for "data oriented" types.
     */
    template<typename T>
    struct List : Span<T> {
        constexpr inline List() : Span<T>(nullptr, 0, 0) {}
        constexpr inline List( Empty ) : List() {}
        constexpr inline List( uint capacity ) : Span<T>(0, 0, 0) {
            if_let (data, allocate_data<T>(capacity)) {
                this->ptr = data;
                this->capacity = capacity;
            }
        }
        constexpr inline List( Copy, Span<T const> to_copy ) : List(to_copy.len) {
            this->len = to_copy.len;
            for (uint i = 0; i < to_copy.len; ++i)
                ::new (this->ptr + i) T(to_copy.ptr[i]);
        }
        constexpr inline List( List const& other ) = delete;
        constexpr inline List( List&& other ) : Span<T>(other) {
            other.ptr = nullptr;
            other.len = 0;
            other.capacity = 0;
        }
        constexpr inline ~List() {
            clear();
            deallocate_data(this->ptr);
        }

        constexpr inline void swap( List& other ) {
            Span<T>::swap(other);
        }

        constexpr inline List& clear() {
            for (uint i = this->len; i-- > 0;)
                this->ptr[i].~T();
            this->len = 0;
            return *this;
        }

        /** Append a single element. */
        template<typename ...Args, typename Self>
        [[nodiscard]] constexpr inline bool append( this Self&& self, Args&&... args ) {
            if (self.len >= self.capacity)
                if (!self.unsafe_increase_capacity(math::max(self.capacity * 2u, 8u)))
                    return false;
            ::new (self.ptr + self.len++) T(std::forward<Args>(args)...);
            return true;
        }

        /** Append a span of elements. */
        template<typename ...Args, typename Self>
        [[nodiscard]] constexpr inline bool append_all( this Self&& self, Span<T const> span ) {
            if (self.len + span.len > self.capacity)
                if (!self.unsafe_increase_capacity(math::max(self.capacity, span.len) * 2u))
                    return false;
            for (uint i = 0; i < span.len; ++i)
                :: new (self.ptr + self.len++) T(span[i]);
            return true;
        }

        template<typename ...Args>
        [[nodiscard]] inline bool resize( uint new_length, Args const&... args ) {
            if (new_length < this->len) {
                for (uint i = this->len; i-- > new_length;)
                    this->ptr[i].~T();
                this->len = new_length;
                return true;
            }
            else if (new_length > this->len) {
                bool success = true;
                if (new_length > this->capacity && !unsafe_increase_capacity(new_length * 2u)) {
                    new_length = this->capacity;
                    success = false;
                }
                for (uint i = this->len; i < new_length; ++i)
                    ::new (this->ptr + i) T(args...);
                this->len = new_length;
                return success;
            }
            else return true;
        }

        [[nodiscard]] constexpr inline bool ensure_capacity( uint min_capacity ) {
            if (min_capacity > this->capacity)
                return unsafe_increase_capacity(min_capacity);
            else return true;
        }

        constexpr inline void erase( uint i ) {
            if (i < this->len) {
                [[likely]];
                this->ptr[i].~T();
                this->len--;
                std::memmove(this->ptr + i, this->ptr + i + 1, this->len - i);
            }
        }

        template<typename F>
        constexpr inline bool remove_if( F const& condition ) {
            for (uint i = 0, len = this->len; i < len; ++i) {
                if (condition(this->ptr[i])) {
                    this->ptr[i].~T();
                    uint i_new = i;
                    for (++i; i < len; ++i) {
                        if (condition(this->ptr[i]))
                            this->ptr[i].~T();
                        else {
                            mem_copy(this->ptr + i_new, this->ptr + i);
                            ++i_new;
                        }
                    }
                    this->len = i_new;
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] constexpr inline bool unsafe_increase_capacity( uint new_capacity ) {
            if_let (data, allocate_move<T>(*this, new_capacity)) {
                this->ptr = data;
                this->capacity = new_capacity;
                return true;
            }
            return false;
        }

        constexpr inline void pop() {
            if (this->len != 0) {
                [[likely]];
                --this->len;
                this->ptr[this->len].~T();
            }
        }
    };
}
