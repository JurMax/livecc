#pragma once
#include "globals.hpp"

#include <set>
#include <filesystem>
#include <set>
#include <atomic>
#include <unordered_map>

namespace fs = std::filesystem;

struct InitData {
    // Store the library header file file times here, so we
    // don't have to keep checking them for changes.
    std::unordered_map<fs::path, fs::file_time_type> file_changes;
    std::mutex mutex;
    std::set<fs::path> system_headers;
};

struct SourceFile {
    enum type_t {
        UNIT,
        PCH,
        SYSTEM_PCH,
        MODULE,
        // HEADER_UNIT,
    };

    fs::path source_path;
    fs::path compiled_path;

    type_t type;

    std::optional<fs::file_time_type> last_write_time;

    fs::path latest_dll;

    // TODO: implement this. Also add support for header units. Used header units
    // should also be added to the source files with the header unit type
    std::string module_name; // if type == MODULE.

    std::set<fs::path> header_dependencies;
    std::set<std::string> system_header_dependencies;
    std::set<std::string> module_dependencies;
    std::string build_pch_includes; // pch includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module or header. These get added to the
    // queue once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<SourceFile*> dependent_files;

    std::atomic<int> compiled_dependencies; // When this is equal to dependencies_count, we can compile.
    int dependencies_count;

public:
    SourceFile(const fs::path& path, type_t type = UNIT);
    SourceFile(SourceFile&& other);

    // Returns true if it must be compiled.
    bool load_dependencies(Context& context, InitData& init_data);

    inline bool is_header() const { return type == PCH || type == SYSTEM_PCH; }

private:
    void create_dependency_files(Context& context, const fs::path& dependencies_path, const fs::path& modules_path);

    // Load the modules .md file.
    void load_module_dependencies(const fs::path& path);
    std::optional<fs::file_time_type> load_header_dependencies(Context& context, const fs::path& path, InitData& init_data);

public:
    // Read the dependencies directly from the file.
    void read_dependencies(Context& context);

public:
    bool has_source_changed();

    std::string get_build_command(const Context& context, bool live_compile = false, fs::path* output_path = nullptr);

    // Returns true if an error occurred.
    bool compile(Context& context, bool live_compile = false);
    void replace_functions(Context& context);
};


// Get the table of string from a link map handle.
std::string_view get_string_table(link_map* handle);
