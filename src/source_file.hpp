#pragma once
#include "context.hpp"

#include <map>
#include <set>
#include <filesystem>
#include <atomic>
#include <optional>

namespace fs = std::filesystem;

struct SourceFile {
    enum type_t {
        UNIT,            // A C++ source file/translation unit
        C_UNIT,          // A C source file/translation unit TODO
        MODULE,          // A C++ module, gets compiled twice.
        HEADER,          // Compiled as header unit
        C_HEADER = HEADER,        // Compiled as header unit
        SYSTEM_HEADER,   // compiled as header unit
        C_SYSTEM_HEADER = SYSTEM_HEADER, // compiled as header unit
        PCH,             // There can be only 1 pch
        BARE_INCLUDE,    // Not compiled, only included
    };

    type_t type;

    fs::path source_path; // always relative to the working directory.
    fs::path compiled_path;

    // If its defined it means the file exists.
    std::optional<fs::file_time_type> source_time;
    std::optional<fs::file_time_type> compiled_time;
    bool has_changed = false;
    bool need_compile = false;
    bool visited = false;
    bool compilation_failed = false;

    // TODO: implement this. Also add support for header units. Used header units
    // should also be added to the source files with the header unit type
    std::string module_name; // only set for modules.

    // Type can be HEADER, SYSTEM_HEADER or INCLUDED_UNIT
    std::map<fs::path, type_t> include_dependencies;
    std::set<std::string> module_dependencies;
    std::vector<char> build_includes; // module includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module or header. These get added to the
    // queue once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<SourceFile*> dependent_files;

    std::atomic<int> compiled_dependencies; // When this is equal to dependencies_count, we can compile.
    int dependencies_count = 0;

    // RUNTIME.
    fs::path latest_obj;

public:
    SourceFile(const Context& context, const fs::path& path, type_t type);

    static std::optional<type_t> get_type(const std::string_view& path);

    inline bool is_header() const { return type == HEADER || type == SYSTEM_HEADER || type == C_HEADER || type == C_SYSTEM_HEADER; }
    inline bool is_include() const { return is_header() || type == PCH || type == BARE_INCLUDE; }

    /** Set the compile path to be inside context.output_directory */
    void set_compile_path(const Context& context);

    /** Read the dependencies directly from the file. */
    void read_dependencies(Context& context);

public:
    /** Check if the file has changed since compilation. */
    bool has_source_changed();

    std::string get_build_command(const Context& context, bool live_compile, fs::path* output_path);

    // Returns true if no errors occurred.
    bool compile(Context& context, bool live_compile = false);
    void replace_functions(Context& context);
};


// Get the table of string from a link map handle.
std::string_view get_string_table(link_map* handle);


// bool file_exists(const char* path);
// std::optional<timespec> get_write_time(const char* path);
