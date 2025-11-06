#include "source_file.hpp"

#include "context.hpp"
#include "module_mapper_pipe.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <format>

using namespace std::literals;

// Make a path lexically normal, and possibly relative to the working directory
static fs::path normalise_path(Context const& context, const fs::path& path) {
    std::error_code err;
    fs::path absolute_path = fs::canonical(path, err);
    if (err)
        return path;
    fs::path relative_path = fs::relative(path, context.working_directory, err);
    if (!err && relative_path.native()[0] != '.')
        return relative_path;
    else
        return absolute_path;
}

/**
 * Get all the include and imports by reading the given file,
 * as well as the module name if it's there.
 * This parser is not checking for valid C/C++ code, it just tries
 * to extract the includes and modules in the fastest possible way.
 */
struct Parser {
    SourceFile& file;
    std::ifstream f; // TODO: use a direct buffer here.

    char buffer[4096];
    size_t buf_len = 0;
    char c;

    Parser(SourceFile& file) : file(file), f(file.source_path) {}

    ErrorCode parse(Context const& context) {
        if (!f.is_open())
            return ErrorCode::OPEN_FAILED;
        enum { INCLUDE, IMPORT, MODULE } read_mode;
        char end_quote;
        c = read_char();

        empty_space: switch (c) { // anything after a newline.
            case EOF: return ErrorCode::OK;
            case '/': parse_comment(); [[fallthrough]];
            case ';': case ' ': case '\r': case '\n': c = read_char(); goto empty_space;
            case '#': if (parse_word("include"sv)) { read_mode = INCLUDE; goto read_start; } else goto token;
            case 'i': if (parse_word(  "mport"sv)) { read_mode =  IMPORT; goto read_start; } else goto token;
            case 'm': if (parse_word(  "odule"sv)) { read_mode =  MODULE; goto read_start; } else goto token;
            default: c = read_char(); goto token;
        }

        token: switch (c) {  // Wait until we hit a newline or a ;
            case EOF: return ErrorCode::OK;
            case '/': parse_comment(); [[fallthrough]];
            case ';': case ' ': case '\r': case '\n': c = read_char(); goto empty_space;
            default: c = read_char(); goto token;
        }

        read_start: switch (c) {
            case EOF: return ErrorCode::UNEXPECTED_END;
            case '/': parse_comment(); [[fallthrough]];
            case ' ': case '\t': case '\n': case '\r': c = read_char(); goto read_start; // skip whitespace
            case ';': c = read_char(); goto empty_space;
            default:
                buffer[0] = c;
                buf_len = 1;
                switch (c) {
                    case '<': end_quote = '>'; goto read_characters_quoted;
                    case '"': end_quote = '"'; goto read_characters_quoted;
                    default: goto read_characters;
                }
        }
        read_characters: switch (c = read_char()) {
            case EOF: return ErrorCode::UNEXPECTED_END;
            case '/': parse_comment(); [[fallthrough]];
            case ';': case ' ': case '\t': case '\n': case '\r': goto write_characters;
            default:
                if (!path_add_char(c)) return ErrorCode::BUFFER_TOO_SMALL;
                goto read_characters;
        }
        read_characters_quoted: switch (c = read_char()) {
            case EOF: return ErrorCode::UNEXPECTED_END;
            default:
                if (!path_add_char(c)) return ErrorCode::BUFFER_TOO_SMALL;
                if (c == end_quote) goto write_characters;
                goto read_characters_quoted;
        }

        write_characters:
            switch (read_mode) {
                case INCLUDE: register_include(context); break;
                case IMPORT: file.parents.emplace_back(std::string{buffer, buf_len}, SourceFile::MODULE); break;
                case MODULE: file.module_name = std::string{buffer, buf_len}; break;
            }
            c = read_char();
            goto empty_space;
    }

    inline bool parse_word( std::string_view word ) {
        for (size_t i = 0; i < word.size(); ++i)
            if ((c = read_char()) != word[i])
                return false;
        switch (c = read_char()) {
            case ' ': case '\t': case '\n': case '\r': case '<': case '"': return true;
            default: return false;
        }
    }

    void parse_comment() {
        c = read_char();
        switch (c) {
            case '/': goto linecomment;
            case '*': goto multicomment_1;
            default: case EOF: return;
        }
        linecomment: switch (c = read_char()) {
            case '\n': case EOF: return;
            default: goto linecomment;
        }
        multicomment_1: switch (c = read_char()) {
            case '*': goto multicomment_2;
            case EOF: return;
            default: goto multicomment_1;
        }
        multicomment_2: switch (c = read_char()) {
            case '/': case EOF: return;
            case '*': goto multicomment_2;
            default: goto multicomment_1;
        }
    }

    inline char read_char() {
        return (char)f.get();
    }

    inline bool path_add_char(char c) {
        buffer[buf_len++] = c;
        return buf_len != sizeof(buffer);
    }

    void register_include(Context const& context) {
        if (buf_len <= 2) return;
        fs::path path(std::string_view(buffer + 1, buf_len - 2));

        if (buffer[0] == '<') {
            file.parents.emplace_back(path, context.use_header_units
                    ? SourceFile::SYSTEM_HEADER_UNIT : SourceFile::SYSTEM_HEADER);
            return;
        }

        if (path.is_absolute()) {
            try_add_include(context, path);
            return;
        }
        if (try_add_include(context, file.source_path.parent_path() / path))
            return;
        for (const std::string_view& dir : context.build_include_dirs)
            if (try_add_include(context, dir / path))
                return;
        if (try_add_include(context, "/usr/local/include" / path) ||
            try_add_include(context, "/usr/include" / path))
            return;
    }
    bool try_add_include(Context const& context, const fs::path& path) {
        std::error_code err;
        if (fs::exists(path, err) && !err) {
            SourceFile::Type type = SourceFile::get_type(path.native()).value_or(SourceFile::BARE_INCLUDE);
            switch (type) {
                case SourceFile::HEADER:
                    if (context.use_header_units)
                        type = SourceFile::HEADER_UNIT;
                    break;
                default:
                    type = SourceFile::BARE_INCLUDE;
                    break;
            }
            file.parents.emplace_back(normalise_path(context, path), type);
            return true;
        }
        return false;
    }
};


SourceFile::SourceFile(Context const& context, const fs::path& path, Type type)
    : type(type), source_path(type != SYSTEM_HEADER && type != SYSTEM_HEADER_UNIT ? normalise_path(context, path) : path) {
}

void SourceFile::set_compile_path(Context const& context) {
    if (type == SYSTEM_HEADER || type == SYSTEM_HEADER_UNIT)
        compiled_path = context.output_directory / "system" / source_path;
    else {
        const char* path = source_path.c_str();
        if (source_path.is_absolute())
            path += source_path.root_path().native().size();
        compiled_path = context.output_directory / path;
    }

    switch (type) {
        case UNIT:
        case C_UNIT:
        case MODULE:
            compiled_path += ".o"; break;
        case HEADER:
        case SYSTEM_HEADER:
        case BARE_INCLUDE:
            compiled_path += ".timestamp"; break;
        case HEADER_UNIT:
        case SYSTEM_HEADER_UNIT:
            compiled_path += ".pcm"; break;
        case PCH:
            compiled_path += ".gch"; break;
    }
}

ErrorCode SourceFile::read_dependencies(Context const& context) {
    std::error_code err;

    if (compiled_path.empty())
        set_compile_path(context);

    // Get the compiled time.
    compiled_time.reset();
    auto compiled_write_time = fs::last_write_time(compiled_path, err);
    if (!err)
        compiled_time = compiled_write_time;
    else
        fs::create_directories(compiled_path.parent_path(), err);

    fs::file_time_type source_write_time;
    if (type == SYSTEM_HEADER || type == SYSTEM_HEADER_UNIT) {
        // Check the system header write time by going through all the options.
        for (const std::string_view& dir : context.build_include_dirs) {
            source_write_time = fs::last_write_time(dir / source_path, err);
            if (!err)
                return ErrorCode::OK;
        }
        for (const fs::path& dir : context.system_include_dirs) {
            source_write_time = fs::last_write_time(dir / source_path, err);
            if (!err)
                return ErrorCode::OK;
        }
        // Not finding system headers is okay, they might be hidden behind a preprocessor.
        return ErrorCode::OK;
    }
    else {
        source_time.reset();
        source_write_time = fs::last_write_time(source_path, err);

        // Get the dependencies of anything but the system headers.
        Parser parser(*this);
        ErrorCode ret = err ? ErrorCode::OPEN_FAILED : parser.parse(context);
        switch (ret) {
            case ErrorCode::OK:
                source_time = source_write_time;
                break;
            case ErrorCode::OPEN_FAILED:
                // Not finding headers is okay, they might be hidden behind a preprocessor.
                if (type == HEADER || type == HEADER_UNIT)
                    ret = ErrorCode::OK;
                else
                    context.log.error("failed to open file ", source_path);
                break;
            case ErrorCode::BUFFER_TOO_SMALL:
                context.log.error("a path or name in ", source_path, " is larger than 4096 characters");
                break;
            default:
                context.log.error("parsing error in ", source_path);
                break;
        }
        if (type == HEADER || type == HEADER_UNIT)
            return ErrorCode::OK;
        return ret;
    }
}


bool SourceFile::has_source_changed() {
    if (is_include())
        return false;
    std::error_code err;
    fs::file_time_type new_source_time = fs::last_write_time(source_path, err);
    if (err)
        return true;
    else {
        source_time = new_source_time;
        return !compiled_time || new_source_time > *compiled_time;
    }
}
bool SourceFile::compile_to_timestamp() const {
    switch (type) {
        case HEADER: case SYSTEM_HEADER: case BARE_INCLUDE: return true;
        default: return false;
    }
};

std::string SourceFile::get_build_command(Context const& context, const fs::path& output_path, bool live_compile) const {
    if (compile_to_timestamp())
        return std::format("touch \"{}\"", output_path.native());

    std::ostringstream command;
    command << context.build_command;
    command << (type == C_UNIT ? context.c_version : context.cpp_version) << ' ';

    if (type != PCH && context.compiler_type == Context::GCC)
        command << "-fmodules ";

    if (context.include_source_parent_dir) {
        if (source_path.has_parent_path())
            command << "-I\"" << source_path.parent_path().native() << "\" ";
        else command << "-I. ";
    }

    command << std::string_view{build_includes.data(), build_includes.size()};

    if (type == PCH)
        command << "-xc++-header -c ";
    else if (type == HEADER_UNIT)
        command << "-fmodule-header=user -xc++-header "; // compile all headers as c++
    else if (type == SYSTEM_HEADER_UNIT)
        command << "-fmodule-header=system -xc++-header ";
    else if (!live_compile) {
        if (type == MODULE) command << "--precompile ";
        else command << "-c ";
    }
    else {
        command << "-shared ";
        if (context.rebuild_with_O0)
            command << "-O0 ";
    }

    command << '"' << source_path.native() << '"';

    // GCC uses the IPC for this. // TODO: check if supplying the output is fine.
    if (context.compiler_type == Context::GCC)
        switch (type) {
            case HEADER_UNIT: case SYSTEM_HEADER_UNIT: case MODULE: return command.str();
            default: break;
        }

    command << " -o \"" << output_path.native() << '"';
    return command.str();
}

std::string_view SourceFile::pch_include() {
    return std::string_view{compiled_path.c_str(), compiled_path.native().size() - 4};
}

/*static*/ std::optional<SourceFile::Type> SourceFile::get_type( const std::string_view& path ) {
    size_t i = path.find_last_of('.');
    if (i != std::string_view::npos) {
        std::string_view ext = path.substr(i + 1);
        if (ext == "c") return {C_UNIT};
        if (ext == "cppm") return {MODULE};
        constexpr char unit_ext[8][4] = {"cc", "cp", "cpp", "cxx", "CPP", "c++", "C"};
        constexpr char head_ext[8][4] = {"hh", "hp", "hpp", "hxx", "HPP", "h++", "H", "h"};
        for (size_t i = 0; i < 8; ++i) {
            if (ext == unit_ext[i]) return {UNIT};
            if (ext == head_ext[i]) return {HEADER};
        }
    }
    return {};
}
