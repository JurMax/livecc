#pragma once
#include "context.hpp"

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;


class SourceFile {
public:
    enum Type {
        UNIT,               // A C++ source file/translation unit
        C_UNIT,             // A C source file/translation unit TODO
        MODULE,             // A C++ module, gets compiled twice.
        HEADER,             // Not compiled
        HEADER_UNIT,        // Compiled as header unit
        SYSTEM_HEADER,      // Not compiled
        SYSTEM_HEADER_UNIT, // compiled as header unit
        PCH,                // Compiled. There can be only 1 pch
        BARE_INCLUDE,       // Not compiled, only included
    };

    Type type;

    fs::path source_path; // always relative to the working directory.
    fs::path compiled_path;

    // If the corresponding time is defined it means the file exists.
    std::optional<fs::file_time_type> source_time;
    std::optional<fs::file_time_type> compiled_time;

    bool need_compile = false;
    bool _temporary; // usable by multiple systems to store some temporary data.

    // TODO: implement this. Also add support for header units. Used header units
    // should also be added to the source files with the header unit type
    std::string module_name; // only set for modules.

    struct Dependency {
        fs::path path;
        Type type;
    };

    // The headers and modules this file depends on.
    std::vector<Dependency> parents;
    std::vector<char> build_includes; // module includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module or header. These get added to the
    // queue once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<uint> children;

public:
    SourceFile(Context const& context, fs::path const& path, Type type);

    inline bool is_include() const {
        switch (type) { case UNIT: case C_UNIT: case MODULE: return false; default: return true; }
    }
    bool compile_to_timestamp() const;

    /** Set the compile path to be inside context.output_directory */
    void set_compile_path(Context const& context);

    /** Read the dependencies directly from the file. Return true on success. */
    ErrorCode read_dependencies(Context const& context);

    /** Check if the file has changed since compilation. */
    bool has_source_changed();

    std::string get_build_command(Context const& context, fs::path const& output_path, bool live_compile) const;
    inline std::string get_build_command(Context const& context) const {
        return get_build_command(context, compiled_path, false);
    }

    // remove ".gch"
    std::string_view pch_include();

    // Get a source type based on a file extension.
    static std::optional<Type> get_type(std::string_view const& path);
};
