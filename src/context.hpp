#pragma once

#include <vector>
#include <filesystem>
#include <mutex>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <format>
#include <cstring>
#include <csignal>

#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dlfcn.h>

namespace fs = std::filesystem;

struct link_map;
typedef struct plthook plthook_t;

enum build_type_t {
    LIVE,         // Build and run as a live application.
    SHARED,       // Build as an optimized shared library.
    STANDALONE,   // Build as a standalone executable.
};

struct Context {
    fs::path working_directory;
    fs::path output_file;
    fs::path output_directory;

    std::string_view compiler = "clang";
    enum { CLANG, GCC } compiler_type = CLANG;

    // Command line arguments.
    build_type_t build_type = LIVE;
    bool include_source_parent_dir = true;
    bool use_header_units = true;
    bool rebuild_with_O0 = false;
    bool verbose = false;
    bool test = false; // make this a build type that just uses the same files.

    bool build_command_changed = false; // Has the build command changed since last invoke.
    std::string build_command;
    std::vector<std::string_view> build_include_dirs;
    std::vector<fs::path> system_include_dirs;

    std::string_view cpp_version = "-std=c++23";
    std::string_view c_version = "-std=c17";

    std::string link_arguments;
    bool custom_linker_set = false;

    // The amount of files to compile in parallel.
    int job_count = 0;

    // Runtime.
    link_map* handle;
    plthook_t* plthook;
    std::vector<void*> loaded_handles;
    std::vector<fs::path> temporary_files;

public:
    std::mutex print_mutex;
    std::string task_name;
    int bar_task_current;
    int bar_task_total;
    int term_width;

public:
    inline Context() {
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        term_width = w.ws_col;
    }
    Context(const Context&) = delete;

    template<typename ...Args>
    void log_info(const Args&... args) {
        std::unique_lock<std::mutex> lock(print_mutex);
        std::ostringstream ss;
        ((ss << args), ...);
        std::string to_print = ss.str();
        std::cout << to_print;
        for (int i = to_print.size() + 1; i < term_width; ++i)
            std::cout << ' ';
        std::cout << std::endl;

        if (!task_name.empty()) {
            print_bar();
        }
    }

    // template<typename ...Args>
    // inline void log_error(const Args&... args) {
    //     log_info(args...);
    // }
    template<typename ...Args>
    inline void log_error(const Args&... args) {
        log_info("\e[1;31mERROR:\e[0m \e[1m",  args..., "\e[0m");
    }

    inline void log_set_task(const std::string_view& task, int task_total) {
        task_name = task;
        bar_task_total = task_total;
        bar_task_current = 0;
    }
    inline void log_clear_task() {
        task_name.clear();
    }
    inline void log_step_task() {
        std::unique_lock<std::mutex> lock(print_mutex);
        ++bar_task_current;
        print_bar();
    }
private:
    inline void print_bar() {
        std::cout << task_name << " [";
        int length = term_width - task_name.length() - 2 - 7;
        int progress = bar_task_current * length / bar_task_total;
        int i = 0;
        for (; i < progress; ++i) std::cout << '=';
        if (i < length) std::cout << '>';
        for (++i; i < length; ++i) std::cout << ' ';
        std::cout << std::format("] {:>3}%\r", bar_task_current * 100 / bar_task_total);
        std::cout.flush();
    }
};
