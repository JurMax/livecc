#pragma once

#include <filesystem>
#include <iostream>
#include <mutex>
#include <string_view>
#include <string>
#include <vector>

#include "base.hpp"

namespace fs = std::filesystem;

struct link_map;
typedef struct plthook plthook_t;

enum class BuildType {
    LIVE,         // Build and run as a live application.
    SHARED,       // Build as an optimized shared library.
    STANDALONE,   // Build as a standalone executable.
};

/**
 * All the settings and logging.
 */
struct Context {
    struct Settings {
        std::string output_name = "app";
        fs::path working_dir = fs::current_path();
        fs::path build_dir = "build";
        fs::path output_dir;
        fs::path output_file;

        std::string_view compiler = "clang";
        enum { CLANG, GCC } compiler_type = CLANG;

        // Command line arguments.
        BuildType build_type = BuildType::LIVE;
        bool include_source_parent_dir = true;
        bool use_header_units = false;
        bool rebuild_with_O0 = false;
        bool verbose = false;
        bool clean = false; // delete the build directory before starting.
        bool test = false; // make this a build type that just uses the same files.
        bool do_compile = true; // if false, stop after making compile_commands.json

        std::string build_command;
        std::vector<std::string_view> build_include_dirs;
        std::vector<fs::path> system_include_dirs; // TODO: move this.

        std::string_view cpp_version = "-std=c++23";
        std::string_view c_version = "-std=c17";

        std::string link_arguments;
        bool custom_linker_set = false;

        // The amount of files to compile in parallel.
        int job_count = 0;
    } settings;

    mutable struct Logging {
        Logging();

        inline void print(const auto&... args) {
            (std::cout << ... << args);
        }

        inline void info(const auto&... args) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!task_name.empty())
                clear_term();
            (std::cout << ... << args) << '\n';
            if (!task_name.empty())
                print_bar();
        }

        inline void error(const auto&... args) {
            std::unique_lock<std::mutex> lock(mutex);
            clear_term();
            std::cerr << "\x1B[1;31mERROR:\x1B[0m \x1B[1m";
            (std::cerr << ... << args) << "\x1B[0m\n";
            if (!task_name.empty())
                print_bar();
        }

        void set_task(const std::string_view& task, int task_total);
        void increase_task_total(int amount = 1);
        void clear_task();
        void step_task();

    private:
        void clear_term() const;
        void print_bar() const;

        std::mutex mutex = {};
        std::string task_name = {};
        int bar_task_current = 0;
        int bar_task_total = 0;
        int term_width = 80;
    } log;
};
