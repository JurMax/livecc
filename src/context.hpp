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
    fs::path working_directory;
    fs::path output_file;
    fs::path output_directory;

    std::string_view compiler = "clang";
    enum { CLANG, GCC } compiler_type = CLANG;

    // Command line arguments.
    BuildType build_type = BuildType::LIVE;
    bool include_source_parent_dir = true;
    bool use_header_units = true;
    bool rebuild_with_O0 = false;
    bool verbose = false;
    bool test = false; // make this a build type that just uses the same files.

    std::string build_command;
    std::vector<std::string_view> build_include_dirs;
    std::vector<fs::path> system_include_dirs;

    std::string_view cpp_version = "-std=c++23";
    std::string_view c_version = "-std=c17";

    std::string link_arguments;
    bool custom_linker_set = false;

    // The amount of files to compile in parallel.
    int job_count = 0;

    mutable struct Logging {
        Logging();

        inline void print(const auto&... args) {
            (std::cout << ... << args);
        }

        inline void info(const auto&... args) {
            std::unique_lock<std::mutex> lock(mutex);
            clear_term();
            (std::cout << ... << args) << '\n';
            print_bar();
        }

        inline void error(const auto&... args) {
            std::unique_lock<std::mutex> lock(mutex);
            clear_term();
            std::cerr << "\e[1;31mERROR:\e[0m \e[1m";
            (std::cerr << ... << args) << "\e[0m\n";
            print_bar();
        }

        void set_task(const std::string_view& task, int task_total);
        void increase_task_total(int amount = 1);
        void clear_task();
        void step_task();

    private:
        void clear_term() const;
        void print_bar() const;

        std::mutex mutex;
        std::string task_name;
        int bar_task_current;
        int bar_task_total;
        int term_width;
    } log;
};
