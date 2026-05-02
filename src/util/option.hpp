#pragma once

#include <cstring>
#include <type_traits>
#include <utility>

#include "base.hpp"

/**
 * An if statement which defines a variable with the given name containing the optional value.
 */
#define if_let(Name, Optional) \
    if (decltype(livecc::option_store_type(Optional)) __ ## Name = Optional) \
        if (decltype(auto) Name = __ ## Name.unsafe_unwrap(); false) \
            {} \
        else

/**
 * Define a variable with the optional value, or return if it has no value.
 */
#define try_let(Name, Optional, Return) \
    decltype(livecc::option_store_type(Optional)) __ ## Name = Optional; \
    if (!__ ## Name) return Return; \
    decltype(auto) Name = __ ## Name.unsafe_unwrap()

/**
 * Continue while an optional contain for a maximum number of tries.
 */
#define while_let(Name, Optional) \
    uint Name ## _loop_count = 0; \
    while (auto __ ## Name = Optional) \
        if (decltype(auto) Name = __ ## Name.unsafe_unwrap(); ++Name ## _loop_count > 10000) { \
            livecc::log_error("max loop bound of 10000 exceeded"); \
            break; \
        } \
        else


namespace livecc {
    template<typename T>
    class Option;

    /**
     * Store if an option contains a value or not.
     */
    template<typename T>
    struct OptionExists {
        constexpr inline OptionExists() : _exists(false) {}
        constexpr inline bool has_value( T const& ) const { return _exists; }
        constexpr inline void create() { _exists = true; }
        constexpr inline void clear( T& ) { _exists = false; }
        bool _exists;
    };

    /**
     * Store the exists flag in the object itself. Needs unsafe_option_get()
     * to return true if the option exists, and unsafe_option_set() to
     * initialise the option state to None.
     */
    template<typename T> requires requires (T t) { t.unsafe_option_get(); t.unsafe_option_set(); }
    struct OptionExists<T> {
        constexpr inline bool has_value( T const& data ) const { return data.unsafe_option_get(); }
        constexpr inline void create() {}
        constexpr inline void clear( T& data ) { data.unsafe_option_set(); }
    };

    template<typename T>
    class Option {
    public:
        constexpr inline Option() { exists.clear(unsafe_value()); }
        constexpr inline Option( None ) : Option() {}

        template<typename ...Args> requires (sizeof...(Args) != 0 && std::is_constructible_v<T, Args&&...>)
        constexpr inline Option( Args&&... args ) {
            exists.create();
            ::new (data) T(std::forward<Args>(args)...);
        }

        constexpr inline explicit Option( Option const& other ) {
            if (other.has_value()) {
                exists.create();
                ::new (data) T(other.unsafe_value());
            }
            else exists.clear(unsafe_value());
        }
        constexpr inline Option( Option&& other ) {
            if (other.has_value()) {
                exists.create();
                other.unsafe_move_to(&unsafe_value());
            }
            else exists.clear(unsafe_value());
        }

        /** Construct from another optional. */
        template<typename U> requires requires (U&& u) { (bool)u; T(u.unsafe_unwrap()); }
        constexpr inline Option( U&& other ) {
            if (other.has_value()) {
                exists.create();
                ::new (data) T(other.unsafe_unwrap());
                other.clear();
            }
            else exists.clear(unsafe_value());
        }
        constexpr inline Option& operator=( Option const& other ) {
            if (has_value()) unsafe_value().~T();
            ::new (this) Option(other);
            return *this;
        }
        constexpr inline Option& operator=( Option&& other ) {
            if (has_value()) unsafe_value().~T();
            ::new (this) Option(std::move(other));
            return *this;
        }
        constexpr inline ~Option() { if (has_value()) unsafe_value().~T(); }

        constexpr inline void unsafe_move_to( T* dest ) {
            std::memcpy((void*)dest, (void*)&unsafe_value(), sizeof(T));
            exists.clear(unsafe_value());
        }

        constexpr inline void clear() {
            if (has_value()) {
                unsafe_value().~T();
                exists.clear(unsafe_value());
            }
        }

        constexpr inline T value_or( T const& default_value ) const {
            return has_value() ? unsafe_value() : default_value;
        }

        template<typename ...Args> requires std::is_constructible_v<T, Args&&...>
        constexpr inline T& construct( Args&&... args ) {
            if (has_value()) unsafe_value().~T();
            else exists.create();
            return *::new (data) T(std::forward<Args>(args)...);
        }

        constexpr inline explicit operator bool() const { return has_value(); }
        constexpr inline bool has_value() const { return exists.has_value(unsafe_value()); }
        constexpr inline T& unsafe_value() { return *static_cast<T*>(static_cast<void*>(data)); }
        constexpr inline T const& unsafe_value() const { return *static_cast<T const*>(static_cast<void const*>(data)); }

        constexpr inline T unsafe_unwrap() && { return std::move(unsafe_value()); }
        constexpr inline T& unsafe_unwrap() & { return unsafe_value(); }
        constexpr inline T const& unsafe_unwrap() const& { return unsafe_value(); }

    private:
        alignas(alignof(T)) char data[sizeof(T)];
        #ifdef _MSC_VER
        [[msvc::no_unique_address]]
        #else
        [[no_unique_address]]
        #endif
        OptionExists<T> exists = {};
    };

    /**
     * Specialise the option for references.
     */
    template<typename T>
    class Option<T&>  {
    public:
        constexpr inline Option() : _ptr(nullptr) {}
        constexpr inline Option( None ) : Option() {}
        constexpr inline Option( T& t ) : _ptr(&t) {}
        constexpr inline Option( T* t ) : _ptr(t) {}

        constexpr inline explicit operator bool() const { return has_value(); }
        constexpr inline bool has_value() const { return _ptr != nullptr; }
        constexpr inline void clear() { _ptr = nullptr; }
        constexpr inline T& unsafe_value() const { return *_ptr; }
        constexpr inline T& unsafe_unwrap() const { return *_ptr; }

        constexpr inline Option<std::remove_const_t<T>> to_owned() {
            if (has_value())
                return {*_ptr};
            return {};
        }

    protected:
        T* _ptr;
    };

    /**
     * Unsigned integers store the empty state as -1.
     */
    template<typename T> requires std::is_unsigned_v<T>
    class Option<T>  {
    public:
        static constexpr T NONE_VALUE = (T)-1;
        constexpr inline Option() : _value(NONE_VALUE) {}
        constexpr inline Option( None ) : Option() {}
        constexpr inline Option( T const& t ) : _value(t) {}

        constexpr inline explicit operator bool() const { return has_value(); }
        constexpr inline bool has_value() const { return _value != NONE_VALUE; }
        constexpr inline void clear() { _value = NONE_VALUE; }
        constexpr inline T& unsafe_value() { return _value; }
        constexpr inline T const& unsafe_value() const { return _value; }
        constexpr inline T unsafe_unwrap() const { return _value; }

        constexpr inline T& construct( T value = (T)0 ) { return _value = value; }

        constexpr inline T value_or( T default_value ) const {
            return has_value() ? unsafe_value() : default_value;
        }

    protected:
        T _value;
    };

    template<typename T> constexpr auto option_store_type( T&&      ) -> T       ;
    template<typename T> constexpr auto option_store_type( T&       ) -> T&      ;
    template<typename T> constexpr auto option_store_type( T const& ) -> T const&;
}
