#include "platform.hpp"

#include "context.hpp"

DLL::DLL( DLL&& other ) : handle(other.handle) {
    other.handle = nullptr;
}
DLL& DLL::operator=( DLL&& other ) {
    close();
    handle = other.handle;
    other.handle = nullptr;
    return *this;
}

bool DLL::is_open() const {
    return handle != nullptr;
}

#ifdef __unix__
    #include <elf.h>
    #include <link.h>
    #include <dlfcn.h>
    #include <sys/ioctl.h>
    #include <unistd.h>

    static DLL open_dll(Context::Logging& log, const char* path, int mode) {
        void* handle = dlopen(path, mode);
        if (handle == nullptr)
            log.error("loading shared library failed: ", dlerror());
        return {handle};
    }

    /*static*/ DLL DLL::open_local(Context::Logging& log, const char* path) {
        return open_dll(log, path, RTLD_LAZY | RTLD_LOCAL);
    }
    /*static*/ DLL DLL::open_global(Context::Logging& log, const char* path) {
        return open_dll(log, path, RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
    }
    /*static*/ DLL DLL::open_deep(Context::Logging& log, const char* path) {
        return open_dll(log, path, RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
    }


    void DLL::close() {
        if (handle != nullptr)
            dlclose(handle);
    }

    void* DLL::symbol(const char* name) {
        return handle ? dlsym(handle, name) : nullptr;
    }

    std::string_view DLL::get_soname() {
        if (handle == nullptr)
            return "";
        const char* str_table = "";
        size_t offset = 0;
        for (auto ptr = ((link_map*)handle)->l_ld; ptr->d_tag; ++ptr) {
            if (ptr->d_tag == DT_STRTAB) str_table = (const char*)ptr->d_un.d_ptr;
            else if (ptr->d_tag == DT_SONAME) offset = ptr->d_un.d_val;
        }
        return str_table + offset;
    }

    std::string_view DLL::string_table() {
        if (handle == nullptr)
            return "";
        const char* str_table = "";
        size_t str_table_size = 0;
        for (auto ptr = ((link_map*)handle)->l_ld; ptr->d_tag; ++ptr) {
            if (ptr->d_tag == DT_STRTAB) str_table = (const char*)ptr->d_un.d_ptr;
            else if (ptr->d_tag == DT_STRSZ) str_table_size = ptr->d_un.d_val;
        }
        return {str_table, str_table_size};
    }

    uint platform::get_terminal_width() {
        #ifdef __unix__
            struct winsize w;
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
            return w.ws_col;
        #else
            return 80;
        #endif
    }
#endif
