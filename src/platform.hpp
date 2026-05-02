#pragma once
#include "context.hpp"
#include "util/base.hpp"
#include "util/list.hpp"
#include "util/string.hpp"

namespace livecc {
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

    struct FileWatcher : NoMove {
        struct Dir {
            String path;
            int handle;
        };
        FileWatcher();
        ~FileWatcher();

        ErrorCode add(Span<char const> dir_path);
        List<String>& poll();

        List<Dir> watching_dirs;
        List<String> changed_files;
        List<char> buffer;
        int handle;
    };

    namespace platform {
        uint get_terminal_width();
    }
}
