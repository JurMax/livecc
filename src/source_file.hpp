#pragma once
#include "context.hpp"

#include <map>
#include <set>
#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

class SourceFile {
public:
    enum type_t {
        UNIT,            // A C++ source file/translation unit
        C_UNIT,          // A C source file/translation unit TODO
        MODULE,          // A C++ module, gets compiled twice.
        HEADER,          // Compiled as header unit
        SYSTEM_HEADER,   // compiled as header unit
        PCH,             // There can be only 1 pch
        BARE_INCLUDE,    // Not compiled, only included
    };

    type_t type;

    fs::path source_path; // always relative to the working directory.
    fs::path compiled_path;

    // If the corresponding time is defined it means the file exists.
    std::optional<fs::file_time_type> source_time;
    std::optional<fs::file_time_type> compiled_time;
    bool has_changed = false;  // source time is newer than compiled time

    bool need_compile;
    bool visited; // temporary variable.

    // TODO: implement this. Also add support for header units. Used header units
    // should also be added to the source files with the header unit type
    std::string module_name; // only set for modules.

    // Type can be HEADER, SYSTEM_HEADER or INCLUDED_UNIT
    struct Dependency {
        fs::path path;
        type_t type;
    };
    std::vector<Dependency> include_dependencies;
    std::vector<std::string> module_dependencies;
    std::vector<char> build_includes; // module includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module or header. These get added to the
    // queue once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<uint> dependent_files;

    // std::atomic<int> compiled_dependencies; // When this is equal to parent_count, we can compile.
    int parent_count = 0;

public:
    SourceFile(const Context& context, const fs::path& path, type_t type);

    static std::optional<type_t> get_type(const std::string_view& path);

    inline bool is_header() const { return type == HEADER || type == SYSTEM_HEADER; }
    inline bool is_include() const { return is_header() || type == PCH || type == BARE_INCLUDE; }
    bool uses_timestamp( const Context& context ) const;

    /** Set the compile path to be inside context.output_directory */
    void set_compile_path(const Context& context);

    /** Read the dependencies directly from the file. */
    void read_dependencies(const Context& context);

    /** Check if the file has changed since compilation. */
    bool has_source_changed();

    std::string get_build_command(const Context& context, const fs::path& output_path, bool live_compile);
    inline std::string get_build_command(const Context& context) {
        return get_build_command(context, compiled_path, false);
    }

    std::string_view pch_include();
};
