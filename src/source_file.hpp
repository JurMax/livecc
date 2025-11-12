#pragma once
#include "context.hpp"

#include <filesystem>
#include <optional>

namespace fs = std::filesystem;

struct SourceType {
    enum Type {
        UNIT,               // A C++ source file/translation unit
        C_UNIT,             // A C source file/translation unit.
        MODULE,             // A C++ module, gets compiled twice.
        HEADER,             // Not compiled
        HEADER_UNIT,        // Compiled as header unit
        SYSTEM_HEADER,      // Not compiled
        SYSTEM_HEADER_UNIT, // compiled as header unit
        PCH,                // Compiled. There can be only 1 C++ pch
        C_PCH,              // Compiled. There can be only 1 C pch
        BARE_INCLUDE,       // Not compiled, only included
        OBJECT,             // Only linked.
    };

    inline constexpr SourceType(Type type) : type(type) {}
    inline constexpr SourceType& operator=(Type new_type) { type = new_type; return *this; }
    inline constexpr operator Type() const { return type; }

    /** Is translation unit */
    inline constexpr bool is_include() const {
        switch (type) { case UNIT: case C_UNIT: case MODULE: case OBJECT: return false; default: return true; }
    }
    inline constexpr bool imports_modules() const {
        switch (type) { case UNIT: case HEADER_UNIT: case MODULE: return true; default: return false; }
    }

    inline constexpr bool is_pch() const {
        switch (type) { case PCH: case C_PCH: return true; default: return false; }
    }

    inline constexpr bool compile_to_timestamp() const {
        switch (type) {
            case HEADER: case SYSTEM_HEADER: case BARE_INCLUDE: case OBJECT: return true;
            default: return false;
        }
    };

    /** Get a source type based on a file extension. */
    static std::optional<SourceType> from_extension(std::string_view const& path);

    Type type;
};

struct InputFile {
    fs::path path;
    SourceType type;
};


class SourceFile {
public:
    SourceType type;

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

    // TODO: move this out of here
    struct Dependency {
        fs::path path;
        SourceType type;
    };

    // The headers and modules this file depends on.
    std::vector<Dependency> dependencies;
    std::vector<char> build_includes; // module includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module or header. These get added to the
    // queue once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<uint> children;
    std::vector<uint> parents;

public:
    SourceFile(Context::Settings const& settings, fs::path const& path, SourceType type);

    /** Read the dependencies directly from the file. Return true on success. */
    ErrorCode read_dependencies(Context const& context);

    /** Check if the file has changed since compilation. */
    bool has_source_changed();

    std::string get_build_command(Context::Settings const& settings, fs::path const& output_path, bool live_compile) const;
    inline std::string get_build_command(Context::Settings const& settings) const {
        return get_build_command(settings, compiled_path, false);
    }

    // remove ".gch"
    std::string_view pch_include();
};
