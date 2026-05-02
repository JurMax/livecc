#include "platform.hpp"

#include "context.hpp"
#include "util/base.hpp"
#include <sys/types.h>

using namespace livecc;

DLL::DLL(DLL&& other) : handle(other.handle) {
    other.handle = nullptr;
}
DLL& DLL::operator=(DLL&& other) {
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
    #include <unistd.h>
    #include <sys/poll.h>
    #include <sys/ioctl.h>
    #include <sys/inotify.h>

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
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        return w.ws_col;
    }


    FileWatcher::FileWatcher() : handle(inotify_init1(IN_NONBLOCK)) {
    }

    ErrorCode FileWatcher::add(Span<char const> path) {
        if (watching_dirs.contains([&] (Dir& el) { return el.path.trim_back('/') == path.trim_back('/'); }))
            return ErrorCode::EXISTS_ALREADY;
        if (!watching_dirs.append())
            return ErrorCode::FAILED;
        Dir& dir = watching_dirs[watching_dirs.len - 1];
        dir.path = path;
        if (!dir.path.ends_with('/')) {
            dir.path.append('/');
        }
        dir.handle = inotify_add_watch(handle, dir.path.ptr, IN_OPEN | IN_CLOSE);
        if (dir.handle == -1) {
            watching_dirs.pop();
            return ErrorCode::FAILED;
        }
        return ErrorCode::OK;
    }
    List<String>& FileWatcher::poll() {
        changed_files.clear();
        pollfd poll_handle = {.fd = handle, .events = POLLIN, .revents = 0};
        int poll_num = ::poll(&poll_handle, 1, 0);
        if (poll_num > 0 && poll_handle.revents & POLLIN) {

            // Read all the events into the buffer.
            buffer.clear();
            while (true) {
                if (!buffer.ensure_capacity(buffer.len + 4096))
                    return changed_files;
                ssize_t len = read(handle, buffer.ptr + buffer.len, buffer.capacity - buffer.len);
                if (len == -1 && errno != EAGAIN)
                    break; // error
                if (len <= 0)
                    break;
                buffer.len += (uint)len;
            }

            inotify_event const* event;
            for (char* ptr = buffer.ptr; ptr + sizeof(inotify_event) <= buffer.end(); ptr += sizeof(inotify_event) + event->len) {
                event = (inotify_event const*)ptr;
                if ((event->mask & (IN_MODIFY | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE)) &&
                    !(event->mask & IN_ISDIR) && event->len > 0)
                    if_let (dir, watching_dirs.first([&] (Dir& dir) { return dir.handle == event->wd; }))
                        if (!changed_files.append(String(dir.path.len + event->len).append(dir.path, (char*)event->name)))
                            return changed_files;
            }
        }

        // for (Dir& dir : watching_dirs)
        return changed_files;
    }

    FileWatcher::~FileWatcher() {
        for (Dir& dir : watching_dirs)
            inotify_rm_watch(handle, dir.handle);
        close(handle);
    }

#else
    uint platform::get_terminal_width() {
        return 80;
    }
#endif
