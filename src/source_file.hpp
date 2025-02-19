#pragma once
#include "globals.hpp"

#include <set>
#include <filesystem>
#include <set>
#include <generator>
#include <atomic>
#include <unordered_map>
#include <map>

namespace fs = std::filesystem;

struct init_data_t {
    // Store the library header file file times here, so we
    // don't have to keep checking them for changes.
    std::unordered_map<fs::path, fs::file_time_type> file_changes;
    std::mutex mutex;
    std::set<fs::path> system_headers;
};

struct source_file_t {
    enum type_t {
        UNIT,
        PCH,
        SYSTEM_PCH,
        MODULE,
        // HEADER_UNIT,
    };

    dll_t& dll;

    fs::path source_path;
    fs::path compiled_path;

    type_t type;
    inline bool typeIsPCH() const { return type == PCH || type == SYSTEM_PCH; }

    std::optional<fs::file_time_type> last_write_time;

    fs::path latest_dll;

    // TODO: implement this. Also add support for header units. Used header units
    // should also be added to the source files with the header unit type
    std::string module_name; // if type == MODULE.

    std::set<fs::path> header_dependencies;
    std::set<std::string> module_dependencies;
    std::string build_pch_includes; // pch includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module. These get added to the queue
    // once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<source_file_t*> dependent_files;

    std::atomic<int> compiled_dependencies; // When this is equal to dependencies_count, we can compile.
    int dependencies_count = 0;

public:
    source_file_t(dll_t& dll, const fs::path& path, type_t type = UNIT);
    source_file_t(const source_file_t& lhs) = delete;
    source_file_t(source_file_t&& lhs);


    // Returns true if it must be compiled.
    bool load_dependencies(init_data_t& init_data);

private:
    void create_dependency_files(const fs::path& dependencies_path, const fs::path& modules_path);

    // Load the modules .md file.
    void load_module_dependencies(const fs::path& path);
    std::optional<fs::file_time_type> load_header_dependencies(const fs::path& path, init_data_t& init_data);

public:
    bool has_source_changed();

    std::string get_build_command(bool live_compile = false, fs::path* output_path = nullptr);

    // Returns true if an error occurred.
    bool compile(bool live_compile = false);
    void replace_functions();

    std::generator<source_file_t&> get_header_dependencies( std::map<fs::path, source_file_t*>& map );
    std::generator<source_file_t&> get_module_dependencies( std::map<std::string, source_file_t*>& map );
};


std::string_view get_string_table(link_map* handle);