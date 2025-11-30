#pragma once
#include "context.hpp"

struct Context;

struct DLL {
    inline DLL(void* handle = nullptr) : handle(handle) {}
    DLL(DLL const&) = delete;
    DLL(DLL&&);
    DLL& operator=(DLL const&) = delete;
    DLL& operator=(DLL&&);
    inline ~DLL() { close(); }

    static DLL open_global(Context::Logging& log, const char* path);
    static DLL open_local(Context::Logging& log, const char* path);
    static DLL open_deep(Context::Logging& log, const char* path);

    inline operator bool() const { return is_open(); }
    bool is_open() const;

    void* symbol(const char* name);
    std::string_view get_soname();
    std::string_view string_table();
    void close();

    void* handle;
};

namespace platform {
    uint get_terminal_width();
}
