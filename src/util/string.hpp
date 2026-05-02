#pragma once
#include <format>
#include <charconv>

#include "memory.hpp"
#include "math.hpp"

namespace livecc {
    struct String;

    /** Get the length of the string in compile time. */
    constexpr inline uint str_length( char const* str ) {
        if (str == nullptr) return 0;
        uint length = 0;
        while (*str++) ++length;
        return length;
    }

    template<typename T>
    inline Option<T> try_string_to( Span<const char> str, auto const&... extra_args );

    /** Convert a string to a value. Returns true on success. */
    template<typename T>
    inline bool string_to( Span<const char> str, T& value, auto const&... extra_args ) {
        if constexpr (std::is_same_v<T, bool>)
            return value = str.len != 0 && (str.ptr[0] == 'T' || str.ptr[0] == 't' || str.ptr[0] == '1');
        else if constexpr (std::is_same_v<String, T>)
            return (value.clear().append(str), true);
        else
            return std::from_chars(str.ptr, str.ptr + str.len, value, extra_args...).ec == std::errc{};
    }

    /** Try to convert a string to a value. */
    template<typename T>
    inline Option<T> try_string_to( Span<const char> str, auto const&... extra_args ) {
        T value;
        if (string_to(str, value, extra_args...))
            return {value};
        else return {};
    }


    /**
     * Always 0 terminated.
     */
    struct String : Span<char> {
        constexpr inline String() : String("") {}
        constexpr inline String( const char* str, uint len ) : Span<char>(const_cast<char*>(str), len, 0) {}
        constexpr inline String( const char* str ) : String(const_cast<char*>(str), str_length(str)) {}
        constexpr inline String( char* str, uint len, uint capacity ) : Span<char>(str, len, capacity) {}
        constexpr inline String( uint capacity ) : Span<char>(allocate_data<char>(capacity + 1u).unsafe_value(), 0, capacity) {
            this->ptr[0] = 0;
        }
        constexpr inline String( String const& other ) = delete;
        constexpr inline String( String&& other ) : Span<char>(other) {
            other.ptr = const_cast<char*>("");
            other.len = 0;
            other.capacity = 0;
        }
        inline String& operator=( String const& other ) {
            *this = Span<char const>(other);
            return *this;
        }
        inline String& operator=( String&& other ) {
            if (is_editable())
                *this = Span<char const>(other);
            else
                swap(other);
            other.clear();
            return *this;
        }
        inline String& operator=( Span<char const> other ) {
            if (other.len != 0) {
                increase_capacity(other.len);
                for (uint i = 0; i < other.len; ++i)
                    this->ptr[i] = other.ptr[i];
                this->ptr[other.len] = '\0';
                this->len = other.len;
            }
            else
                clear();
            return *this;
        }
        constexpr inline ~String() {
            if !consteval {
                if (is_editable())
                    deallocate_data(this->ptr);
            }
        }

        constexpr inline void swap( String& other ) {
            Span<char>::swap(other);
        }

        inline void make_editable() {
            if (!is_editable()) {
                this->ptr = allocate_copy<char>({this->ptr, this->len + 1u}, this->len + 1u).unsafe_value();
                this->capacity = this->len;
            }
        }

        template<typename ...Ts, typename Self>
        inline Self append( this Self&& self, Ts const&... value ) {
            (([&] {
                if constexpr (requires { Span<char const>(value); }) {
                    Span<char const> span(value);
                    if (self.len + span.len >= self.capacity) // >= to account for 0 capacity.
                        self.unsafe_increase_capacity(math::max(self.len, span.len, 2u) * 2u);
                    memcpy(self.ptr + self.len, span.ptr, span.len);
                    self.len += span.len;
                }
                else if constexpr (std::is_same_v<Ts, char>) self.append_c(value);
                else std::format_to(self.format_iterator(), "{}", value);
            }()), ...);
            self.ptr[self.len] = 0;
            return std::forward<Self>(self);
        }

        template<typename Self>
        inline Self append_c( this Self&& self, char c, uint count = 1) {
            uint new_len = self.len + count;
            if (self.len + count >= self.capacity) // >= to account for 0 capacity.
                self.unsafe_increase_capacity(math::max(self.len, count, 2u) * 2u);
            for (uint i = self.len; i < new_len; ++i)
                self.ptr[i] = c;
            self.len = new_len;
            self.ptr[new_len] = 0;
            return std::forward<Self>(self);
        }

        inline void pop( uint count ) {
            make_editable();
            if (count >= this->len) this->len = 0;
            else                    this->len -= count;
            this->ptr[this->len] = 0;
        }

        inline void increase_capacity( uint min_capacity ) {
            if (min_capacity > this->capacity)
                unsafe_increase_capacity(min_capacity);
        }

        inline void unsafe_increase_capacity( uint new_capacity ) {
            // todo: account for allocate failure
            char* new_data = allocate_copy<char>({ptr, len + 1}, new_capacity + 1u).unsafe_value();
            if (is_editable())
                deallocate_data(this->ptr);
            this->ptr = new_data;
            this->capacity = new_capacity;
        }

        inline String& clear() {
            if (this->len != 0) {
                this->len = 0;
                if (is_editable())
                    this->ptr[0u] = 0;
                else
                    this->ptr = const_cast<char*>("");
            }
            return *this;
        }

        inline String clone() const {
            return {allocate_copy<char>({this->ptr, this->len + 1u}, this->len + 1u).unsafe_value(), this->len, this->len};
        }
        inline String reference() const {
            return {this->ptr, this->len};
        }

        constexpr inline bool is_editable() const { return this->capacity != 0; }

        /**
         * Enable using std::format_to with strings.
         */
        struct FormatIterator {
            String* str;
            constexpr inline FormatIterator( String& str ) : str(&str) {
                if (str.len >= str.capacity)
                    str.unsafe_increase_capacity(math::max(str.len, 8u) * 2u);
            }
            constexpr inline char& operator*() const { return str->ptr[str->len]; }
            constexpr inline FormatIterator& operator++() {
                if (str->len >= str->capacity) // todo: account for mem failure
                    str->unsafe_increase_capacity(str->len * 2u);
                str->len += 1;
                return *this;
            }
            FormatIterator operator++(int); // required but not actually implemented.
            using difference_type = int; // required to satisfy std::format constraint.
        };
        constexpr inline FormatIterator format_iterator() { return {*this}; }


        constexpr inline Span<char const> filename() const {
            if_let (i, find_last('/'))
                return slice(i + 1, this->len);
            else if_let (i, find_last('\\'))
                return slice(i + 1, this->len);
            else return *this;
        }
    };
}

template<>
struct std::formatter<livecc::String> : std::formatter<string_view> {
    inline auto format(livecc::String const& span, std::format_context& ctx) const {
        return std::formatter<string_view>::format({span.ptr, span.len}, ctx);
    }
};

inline std::ostream& operator<<(std::ostream& out, livecc::String const& str) {
    return out << std::string_view{str.ptr, str.len};
}
