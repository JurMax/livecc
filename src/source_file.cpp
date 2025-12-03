#include "source_file.hpp"

#include "context.hpp"
#include "module_mapper_pipe.hpp"
#include "platform.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <format>

using namespace std::literals;

/*static*/ std::optional<SourceType> SourceType::from_extension(std::string_view const& path) {
    size_t i = path.find_last_of('.');
    if (i != std::string_view::npos) {
        SourceType type = HEADER;
        if (++i == path.size()) return {};
        switch (path[i]) {
        // c, cc, cpp, c++
            case 'c': case 'C':
                if (++i == path.size()) return {C_UNIT};
                type = UNIT;
                break;
        // h, hh, hpp, h++
            case 'h': case 'H':
                if (++i == path.size()) return {HEADER};
                type = HEADER;
                break;
        // a
            case 'a': if (path.substr(i) ==   "a") return {STATIC_LIBRARY}; else return {};
            case 'A': if (path.substr(i) ==   "A") return {STATIC_LIBRARY}; else return {};
        // // dll
        //     case 'd': if (path.substr(i) == "dll") return {SHARED_LIBRARY}; else return {};
        //     case 'D': if (path.substr(i) == "DLL") return {SHARED_LIBRARY}; else return {};
        // so
            case 's': if (path.substr(i) ==  "so") return {SHARED_LIBRARY}; else return {};
            case 'S': if (path.substr(i) ==  "SO") return {SHARED_LIBRARY}; else return {};
        /* .so.0.1 */
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                for (size_t i = path.size(); i-- > 0;)
                    switch (path[i]) {
                        case '0': case '1': case '2': case '3': case '4':
                        case '5': case '6': case '7': case '8': case '9': case '.':
                            break;
                        case 'o':
                            if (i > 2 && path[--i] == 's' && path[--i] == '.')
                                return {SHARED_LIBRARY};
                            return {};
                        default: return {};
                    }
                break;
        // o, ob, obj
            case 'o': case 'O':
                if (++i == path.size()) return {OBJECT};
                if (path[i] == 'b' || path[i] == 'B') {
                    if (++i == path.size()) return {OBJECT};
                    if (path[i] == 'j' || path[i] == 'J')
                        if (++i == path.size()) return {OBJECT};
                }
                return {};
            default: return {};
        }

        // Parse the rest of header or source file extensions.
        switch (path[i]) {
            default: break;
            case 'h': case 'H': if (++i == path.size()) return {HEADER}; break;
            case 'c': case 'C': if (++i == path.size()) return   {type}; break;
            case '+': case 'x': case 'X': case 'p': case 'P':
                char letter = path[i];
                if (++i == path.size() || path[i] == letter)
                    return {type};
                break;
        }
    }
    return {};
}


// Make a path lexically normal, and possibly relative to the working directory
static fs::path normalise_path(Context::Settings const& settings, const fs::path& path) {
    std::error_code err;
    fs::path absolute_path = fs::canonical(path, err);
    if (err)
        return path;
    fs::path relative_path = fs::relative(path, settings.working_dir, err);
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
    SourceFile& file; // TODO: dont use the entire file.
    std::ifstream f; // TODO: use a direct buffer here.

    char buffer[4096];
    size_t buf_len = 0;
    char c;

    Parser(SourceFile& file) : file(file), f(file.source_path) {}

    ErrorCode parse(Context::Settings const& settings) {
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
                case INCLUDE: register_include(settings); break;
                case IMPORT: file.dependencies.emplace_back(std::string{buffer, buf_len}, SourceType::MODULE); break;
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

    void register_include(Context::Settings const& settings) {
        if (buf_len <= 2) return;
        fs::path path(std::string_view(buffer + 1, buf_len - 2));

        if (buffer[0] == '<') {
            file.dependencies.emplace_back(path, settings.use_header_units
                    ? SourceType::SYSTEM_HEADER_UNIT : SourceType::SYSTEM_HEADER);
            return;
        }

        if (path.is_absolute()) {
            try_add_include(settings, path);
            return;
        }
        if (try_add_include(settings, file.source_path.parent_path() / path))
            return;
        for (const std::string_view& dir : settings.build_include_dirs)
            if (try_add_include(settings, dir / path))
                return;
        if (try_add_include(settings, "/usr/local/include" / path) ||
            try_add_include(settings, "/usr/include" / path))
            return;
    }
    bool try_add_include(Context::Settings const& settings, const fs::path& path) {
        std::error_code err;
        if (fs::exists(path, err) && !err) {
            SourceType::Type type = SourceType::from_extension(path.native()).value_or(SourceType::BARE_INCLUDE);
            switch (type) {
                case SourceType::HEADER:
                    if (settings.use_header_units)
                        type = SourceType::HEADER_UNIT;
                    break;
                default:
                    type = SourceType::BARE_INCLUDE;
                    break;
            }
            file.dependencies.emplace_back(normalise_path(settings, path), type);
            return true;
        }
        return false;
    }
};


SourceFile::SourceFile(Context::Settings const& settings, const fs::path& path, SourceType type)
    : type(type), source_path(type != SourceType::SYSTEM_HEADER && type != SourceType::SYSTEM_HEADER_UNIT ? normalise_path(settings, path) : path) {

    if (type == SourceType::SYSTEM_HEADER || type == SourceType::SYSTEM_HEADER_UNIT)
        compiled_path = settings.output_dir / "system" / source_path;
    else {
        const char* path = source_path.c_str();
        if (source_path.is_absolute())
            path += source_path.root_path().native().size();
        compiled_path = settings.output_dir / path;
    }

    switch (type) {
        case SourceType::UNIT:
        case SourceType::C_UNIT:
        case SourceType::MODULE:
            compiled_path += ".o";
            break;
        case SourceType::HEADER_UNIT:
        case SourceType::SYSTEM_HEADER_UNIT:
            compiled_path += ".pcm";
            break;
        case SourceType::PCH:
        case SourceType::C_PCH:
            compiled_path += ".gch";
            break;
        case SourceType::HEADER:
        case SourceType::SYSTEM_HEADER:
        case SourceType::BARE_INCLUDE:
        case SourceType::OBJECT:
        case SourceType::STATIC_LIBRARY:
            compiled_path += ".timestamp";
            break;
        case SourceType::SHARED_LIBRARY:
        case SourceType::RESOURCE:
            // Shared library gets updated later to match the libraries SONAME.
            compiled_path = settings.build_dir / source_path.filename();
            break;
    }
}

ErrorCode SourceFile::read_dependencies(Context const& context) {
    std::error_code err;

    source_time.reset();
    compiled_time.reset();
    ErrorCode ret = ErrorCode::OK;
    switch (type) {
        case SourceType::UNIT:
        case SourceType::C_UNIT:
        case SourceType::MODULE:
        case SourceType::HEADER:
        case SourceType::HEADER_UNIT:
        case SourceType::PCH:
        case SourceType::C_PCH:
        case SourceType::BARE_INCLUDE: {
            // Read the dependencies of anything but the system headers.
            fs::file_time_type source_write_time = fs::last_write_time(source_path, err);
            Parser parser(*this);
            ret = err ? ErrorCode::OPEN_FAILED : parser.parse(context.settings);
            switch (ret) {
                case ErrorCode::OK:
                    source_time = source_write_time;
                    break;
                case ErrorCode::OPEN_FAILED:
                    // Not finding headers is okay, they might be hidden behind a preprocessor.
                    if (type == SourceType::HEADER || type == SourceType::HEADER_UNIT)
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
            break;
        }
        case SourceType::SYSTEM_HEADER:
        case SourceType::SYSTEM_HEADER_UNIT: {
            // Check the system header write time by going through all the options.
            for (const std::string_view& dir : context.settings.build_include_dirs) {
                fs::file_time_type source_write_time = fs::last_write_time(dir / source_path, err);
                if (!err)
                    source_time = source_write_time;
            }
            for (const fs::path& dir : context.settings.system_include_dirs) {
                fs::file_time_type source_write_time = fs::last_write_time(dir / source_path, err);
                if (!err)
                    source_time = source_write_time;
            }
            // Not finding system headers is okay, they might be hidden behind a preprocessor.
            break;
        }
        case SourceType::OBJECT: {
            fs::file_time_type source_write_time = fs::last_write_time(source_path, err);
            if (!err) source_time = source_write_time;
            else ret = ErrorCode::OPEN_FAILED;
            break;
        }
        case SourceType::STATIC_LIBRARY:
        case SourceType::SHARED_LIBRARY: {
            fs::file_time_type source_write_time = fs::last_write_time(source_path, err);
            if (!err)
                source_time = source_write_time;
            else {
                ret = ErrorCode::OPEN_FAILED;

                // Search /usr/lib/ for the library.
                if (source_path.is_relative()) {
                    fs::path absolute_path = "/usr/lib" / source_path;
                    source_write_time = fs::last_write_time(absolute_path, err);
                    if (!err) {
                        source_path = absolute_path;
                        source_time = source_write_time;
                        ret = ErrorCode::OK;
                    }
                }
            }

            // Update the compiled path to match the SONAME of the library.
            if (type == SourceType::SHARED_LIBRARY)
                if (DLL dll = DLL::open_local(context.log, source_path.c_str()))
                    compiled_path = context.settings.build_dir / dll.get_soname();
            break;
        }
        case SourceType::RESOURCE:
            return ErrorCode::OK;
    }

    // Get the compiled time.
    auto compiled_write_time = fs::last_write_time(compiled_path, err);
    if (!err)
        compiled_time = compiled_write_time;

    return ret;
}


bool SourceFile::has_source_changed() {
    if (type.is_include())
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
std::string SourceFile::get_build_command(Context::Settings const& settings, const fs::path& output_path, bool live_compile) const {
    if (type.compile_to_timestamp())
        return std::format("touch \"{}\"", output_path.native());
    else if (type == SourceType::SHARED_LIBRARY)
        return std::format("cp -f \"{}\" \"{}\"", source_path.native(), output_path.native());

    std::ostringstream command;
    command << settings.build_command;
    command << (type == SourceType::C_UNIT ? settings.c_version : settings.cpp_version) << ' ';

    if (settings.compiler_type == Context::Settings::GCC && type.uses_modules())
        command << "-fmodules ";

    if (settings.include_source_parent_dir) {
        if (source_path.has_parent_path())
            command << "-I\"" << source_path.parent_path().native() << "\" ";
        else command << "-I. ";
    }

    command << std::string_view{build_includes.data(), build_includes.size()};

    if (type == SourceType::PCH)
        command << "-xc++-header -c ";
    else if (type == SourceType::C_PCH)
        command << "-xc-header -c ";
    else if (type == SourceType::HEADER_UNIT)
        command << "-fmodule-header=user -xc++-header "; // compile all headers as c++
    else if (type == SourceType::SYSTEM_HEADER_UNIT)
        command << "-fmodule-header=system -xc++-header ";
    else if (!live_compile) {
        if (type == SourceType::MODULE) command << "--precompile ";
        else command << "-c ";
    }
    else {
        command << "-shared ";
        if (settings.rebuild_with_O0)
            command << "-O0 ";
    }

    command << '"' << source_path.native() << '"';

    // GCC uses the IPC for this. // TODO: check if supplying the output is fine.
    if (settings.compiler_type == Context::Settings::GCC)
        switch (type) {
            case SourceType::HEADER_UNIT: case SourceType::SYSTEM_HEADER_UNIT: case SourceType::MODULE: return command.str();
            default: break;
        }

    command << " -o \"" << output_path.native() << '"';
    return command.str();
}

std::string_view SourceFile::filename() {
    std::string_view view{source_path.native()};
    size_t i = view.rfind('/');
    if (i != std::string_view::npos)
        view = view.substr(i + 1);
    i = view.rfind('\\');
    if (i != std::string_view::npos)
        view = view.substr(i + 1);
    return view;
}

std::string_view SourceFile::pch_include() {
    std::string_view view{compiled_path.native()};
    return view.ends_with(".gch"sv) ? view.substr(0, view.size() - 4) : view;
}

std::string_view SourceFile::lib_include() {
    std::string_view view{filename()};
    size_t i = view.rfind('.');
    if (view.starts_with("lib"s) && i != std::string_view::npos)
        view = view.substr(3, i - 3);
    return view;
}
