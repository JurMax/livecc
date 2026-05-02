#pragma once

#include "span.hpp"
#include <cstring>

namespace livecc {
    template<typename T>
    void mem_copy( T* dest, T const* source, uint count = 1u ) {
        std::memcpy((void*)dest, (void const*)source, count * sizeof(T));
    }
    template<typename T>
    void mem_move( T* dest, T* source, uint count ) {
        std::memmove((void*)dest, (void*)source, count * sizeof(T));
    }

    [[nodiscard]] Option<void*> allocate_data( uint length, uint alignment );
    template<typename T>
    [[nodiscard]] constexpr inline Option<T*> allocate_data( uint size = 1u ) {
        if_let (data, allocate_data(size * sizeof(T), alignof(T)))
            return {(T*)data};
        return {};
    }
    void deallocate_data( void* data );

    template<typename T>
    Option<T*> allocate_copy( Span<const T> source, uint new_length ) {
        if_let (data, allocate_data<T>(new_length)) {
            if (source.len != 0)
                mem_copy(data, source.ptr, source.len);
            return {data};
        }
        return {};
    }

    template<typename T>
    Option<T*> allocate_move( Span<T> source, uint new_length ) {
        if_let (new_data, allocate_data<T>(new_length)) {
            if (source.len != 0)
                mem_copy(new_data, source.ptr, source.len);
            deallocate_data(source.ptr);
            return {new_data};
        }
        return {};
    }

    template<class T>
    struct HasVTable {
        struct Derived : T { virtual void _force_the_vtable() {} };
        static constexpr inline bool value = sizeof(T) == sizeof(Derived);
    };

    struct Cast {};

    template<typename T>
    struct UniquePtr {
        T* ptr;

        template<typename ...Args>
        static inline Option<UniquePtr> create( Args&&... args ) {
            if_let (data, allocate_data<T>()) {
                ::new (data) T(std::forward<Args>(args)...);
                return {Unsafe{}, data};
            }
            return {};
        }
        constexpr inline UniquePtr( Unsafe, T* ptr ) : ptr(ptr) {}
        template<typename U> requires HasVTable<U>::value
        UniquePtr( Cast, UniquePtr<U>&& other ) : ptr(other.ptr) { other.ptr = nullptr; }
        UniquePtr( UniquePtr& ) = delete;
        UniquePtr( UniquePtr const& ) = delete;
        UniquePtr( UniquePtr&& other ) : ptr(other.ptr) { other.ptr = nullptr; }
        ~UniquePtr() {
            if (ptr != nullptr) {
                ptr->~T();
                deallocate_data(ptr);
            }
        }

        inline void swap( UniquePtr& other ) {
            T* temp = ptr;
            ptr = other.ptr;
            other.ptr = temp;
        }

        T& operator*() const { return *ptr; }
        T* operator->() const { return ptr; }

        constexpr inline void unsafe_option_set() { ptr = nullptr; }
        constexpr inline bool unsafe_option_get() const { return ptr != nullptr; }

    };
}
