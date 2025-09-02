#include "source_file.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <elf.h>
#include <link.h>
#include <string_view>
#include <system_error>
#include <format>

#include "context.hpp"
#include "module_mapper_pipe.hpp"
#include "plthook/plthook.h"

using namespace std::literals;

// Make a path lexically normal, and possibly relative to the working directory
static fs::path normalise_path(const Context& context, const fs::path& path) {
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
    enum Return { OK, OPEN_FAILED, UNEXPECTED_END, PATH_TOO_LONG };

    SourceFile& file;
    char buffer[4096];
    size_t buf_len = 0;

    std::ifstream f; // TODO: use a direct buffer here.
    char c;

    Parser(SourceFile& file) : file(file), f(file.source_path) {
    }

    Return parse(const Context& context) {
        if (!f.is_open())
            return OPEN_FAILED;
        enum { INCLUDE, IMPORT, MODULE } read_mode;
        char end_quote;
        bool got_space;
        c = read_char();

        empty_space: switch (c) { // anything after a newline.
            case EOF: case '{': case '(': return OK;
            case '/': parse_comment(); [[fallthrough]];
            case ';': case ' ': case '\r': case '\n': c = read_char(); goto empty_space;
            case '#': goto include_i;
            case 'i': goto import_m;
            case 'm': goto module_o;
            default: c = read_char(); goto token;
        }

        token: switch (c) {  // Wait until we hit a newline or a ;
            case EOF: case '{': case '(': return OK;
            case '/': parse_comment(); [[fallthrough]];
            case ';': case ' ': case '\r': case '\n': c = read_char(); goto empty_space;
            default: c = read_char(); goto token;
        }

        include_i: if ((c = read_char()) == 'i') goto include_n; else goto token;
        include_n: if ((c = read_char()) == 'n') goto include_c; else goto token;
        include_c: if ((c = read_char()) == 'c') goto include_l; else goto token;
        include_l: if ((c = read_char()) == 'l') goto include_u; else goto token;
        include_u: if ((c = read_char()) == 'u') goto include_d; else goto token;
        include_d: if ((c = read_char()) == 'd') goto include_e; else goto token;
        include_e: if ((c = read_char()) == 'e') { read_mode = INCLUDE; goto read_start; } else goto token;

        import_m: if ((c = read_char()) == 'm') goto import_p; else goto token;
        import_p: if ((c = read_char()) == 'p') goto import_o; else goto token;
        import_o: if ((c = read_char()) == 'o') goto import_r; else goto token;
        import_r: if ((c = read_char()) == 'r') goto import_t; else goto token;
        import_t: if ((c = read_char()) == 't') { read_mode = IMPORT; goto read_start; } else goto token;

        module_o: if ((c = read_char()) == 'o') goto module_d; else goto token;
        module_d: if ((c = read_char()) == 'd') goto module_u; else goto token;
        module_u: if ((c = read_char()) == 'u') goto module_l; else goto token;
        module_l: if ((c = read_char()) == 'l') goto module_e; else goto token;
        module_e: if ((c = read_char()) == 'e') { read_mode = MODULE; goto read_start; } else goto token;

        read_start: got_space = false; goto read_spaces;
        read_spaces: switch (c = read_char()) {
            case EOF: return UNEXPECTED_END;
            case '/': parse_comment(); [[fallthrough]];
            case ' ': case '\t': case '\n': case '\r': got_space = true; goto read_spaces;
            default: if (!got_space) goto token; [[fallthrough]];
            case '"': case '<':
                buffer[0] = c;
                buf_len = 1;
                switch (c) {
                    case '<': end_quote = '>'; goto read_characters_quoted;
                    case '"': end_quote = '"'; goto read_characters_quoted;
                    default: goto read_characters;
                }
        }
        read_characters: switch (c = read_char()) {
            case EOF: return UNEXPECTED_END;
            case '/': parse_comment(); [[fallthrough]];
            case ';': case ' ': case '\t': case '\n': case '\r': goto write_characters;
            default:
                if (!path_add_char(c)) return PATH_TOO_LONG;
                goto read_characters;
        }
        read_characters_quoted: switch (c = read_char()) {
            case EOF: return UNEXPECTED_END;
            default:
                if (!path_add_char(c)) return PATH_TOO_LONG;
                if (c == end_quote) goto write_characters;
                goto read_characters_quoted;
        }

        write_characters:
            switch (read_mode) {
                case INCLUDE: register_include(context); break;
                case IMPORT: file.module_dependencies.insert(std::string{buffer, buf_len}); break;
                case MODULE: file.module_name = std::string{buffer, buf_len}; break;
            }
            c = read_char();
            goto empty_space;
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

    void register_include(const Context& context) {
        if (buf_len <= 2) return;
        fs::path path(std::string_view(buffer + 1, buf_len - 2));

        if (buffer[0] == '<') {
            file.include_dependencies.emplace(path, SourceFile::SYSTEM_HEADER);
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
    bool try_add_include(const Context& context, const fs::path& path) {
        std::error_code err;
        if (fs::exists(path, err) && !err) {
            auto type = SourceFile::get_type(path.native());
            if (!type || (*type != SourceFile::HEADER))
                type = SourceFile::BARE_INCLUDE;
            file.include_dependencies.emplace(normalise_path(context, path), *type);
            return true;
        }
        return false;
    }
};


SourceFile::SourceFile(const Context& context, const fs::path& path, type_t type)
    : type(type), source_path(type != SYSTEM_HEADER ? normalise_path(context, path) : path) {
}

/*static*/ std::optional<SourceFile::type_t> SourceFile::get_type( const std::string_view& path ) {
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

void SourceFile::set_compile_path(const Context& context) {
    if (type == SYSTEM_HEADER)
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
            compiled_path += context.use_header_units ? ".pcm" : ".timestamp"; break;
        case PCH:
            compiled_path += ".gch"; break;
        case BARE_INCLUDE:
            compiled_path += ".timestamp";
            break;
    }
}

void SourceFile::read_dependencies(const Context& context) {
    if (compiled_path.empty())
        set_compile_path(context);

    std::error_code err;
    fs::file_time_type source_write_time;
    if (type == SYSTEM_HEADER) {
        // Check the system header write time by going through all the options.
        for (const std::string_view& dir : context.build_include_dirs) {
            source_write_time = fs::last_write_time(dir / source_path, err);
            if (!err)
                goto found_file;
        }
        for (const fs::path& dir : context.system_include_dirs) {
            source_write_time = fs::last_write_time(dir / source_path, err);
            if (!err)
                goto found_file;
        }
        found_file: ;
    }
    else {
        source_write_time = fs::last_write_time(source_path, err);

        // Get the dependencies of anything but the system headers.
        Parser parser(*this);
        switch (parser.parse(context)) {
            case Parser::OK: break;
            case Parser::OPEN_FAILED:
                // context.log_error("failed to open file ", source_path);
                break;
            case Parser::UNEXPECTED_END:
                context.log_error("parsing error in ", source_path);
                break;
            case Parser::PATH_TOO_LONG:
                context.log_error("a path or name in ", source_path, " is larger than 4096 characters");
                break;
        }
    }

    if (!err)
        source_time = source_write_time;

    auto compiled_write_time = fs::last_write_time(compiled_path, err);
    if (!err) {
        compiled_time = compiled_write_time;
        has_changed = !source_time || source_time > compiled_time;
    }
    else {
        fs::create_directories(compiled_path.parent_path(), err);
        has_changed = true;
    }
}


bool SourceFile::has_source_changed() {
    if (is_include())
        return false;
    std::error_code err;
    fs::file_time_type new_write_time = fs::last_write_time(source_path, err);
    if (err)
        return true;
    if (!compiled_time || *compiled_time < new_write_time) {
        compiled_time = new_write_time;
        return true;
    }
    return false;
}

inline bool use_timestamp(const Context& context, const SourceFile& f) {
    return f.type == SourceFile::BARE_INCLUDE || (!context.use_header_units && f.is_header());
};

std::string SourceFile::get_build_command(const Context& context, const fs::path& output_path, bool live_compile) {
    if (use_timestamp(context, *this))
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
    else if (type == HEADER)
        command << "-fmodule-header=user -xc++-header "; // compile all headers as c++
    else if (type == SYSTEM_HEADER)
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

    // Maybe use the gcm cache
    if (!(context.use_header_units && context.compiler_type == Context::GCC && is_header()))
        command << " -o \"" << output_path.native() << '"';
    // else
    //     command << " && touch \"" << output_path.native() << '"';
    return command.str();
}

std::string_view SourceFile::pch_include() {
     // remove ".gch"
    return std::string_view{compiled_path.c_str(), compiled_path.native().size() - 4};
}

// Returns true if an error occurred. // TODO: move to compiler.cpp
bool SourceFile::compile(Context& context, bool live_compile) {
    std::error_code ec;

    // Get the output path. This differs from compile_path if live compile is true.
    fs::path output_path;
    bool do_live_compile = live_compile && type == UNIT;
    if (do_live_compile) {
        output_path = context.output_directory / "tmp"
            / ("tmp" + (std::to_string(context.temporary_files.size()) + ".so"));
        context.temporary_files.push_back(latest_obj);
    }
    else output_path = compiled_path;

    std::optional<ModuleMapperPipe> pipe;
    std::string build_command = get_build_command(context, output_path, do_live_compile);

    // Create PCH file.
    if (type == SourceFile::PCH) {
        if (context.compiler_type == Context::GCC)
            fs::copy_file(source_path, fs::path{pch_include()}, fs::copy_options::overwrite_existing, ec);
        else
            std::ofstream{fs::path{pch_include()}} << "#error PCH not included\n";
    }
    else if (context.compiler_type == Context::GCC && !use_timestamp(context, *this)) {
        pipe.emplace(context, *this);
        build_command += pipe->mapper_arg();
    }

    if (context.verbose)
        context.log_info("Compiling ", source_path, " to ", output_path, " using: ", build_command);
    else
        context.log_info("Compiling ", source_path, " to ", output_path);

    // Run the command, and exit when interrupted.
    int err = system(build_command.c_str());
    if (err != 0)
        compilation_failed = true;

    if (WIFSIGNALED(err) && (WTERMSIG(err) == SIGINT || WTERMSIG(err) == SIGQUIT))
        exit(1); // TODO: this is ugly, we have to shut down gracefully.

    if (compilation_failed) {
        fs::remove(compiled_path, ec);
        return false;
    }

    latest_obj = output_path;
    compiled_time = fs::last_write_time(compiled_path, ec);
    return true;
}

void SourceFile::replace_functions(Context& context) {
    link_map* handle = (link_map*)dlopen(latest_obj.c_str(), RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
    if (handle == nullptr) {
        context.log_info("Error loading ", latest_obj);
        return;
    }

    context.loaded_handles.push_back(handle);
    std::string_view str_table = get_string_table(handle);

    for (size_t i = 0, l = str_table.size() - 1; i < l; ++i) {
        if (str_table[i] == '\0') {
            const char* name = &str_table[i + 1];
            void* func = dlsym(handle, name);
            // std::cout << name << std::endl;

            if (func != nullptr && strlen(name) > 3) {
                bool is_cpp = name[0] == '_' && name[1] == 'Z';
                if (is_cpp) {
                    int error = plthook_replace(context.plthook, name, func, NULL);
                    (void)error;

                // if (error == 0)
                    // std::cout << "        " << error << std::endl;
                }
            }
        }
    }
}

std::string_view get_string_table(link_map* handle) {
    size_t str_table_size = 0;
    const char* str_table = nullptr;
    for (auto ptr = handle->l_ld; ptr->d_tag; ++ptr) {
        if (ptr->d_tag == DT_STRTAB) str_table = (const char*)ptr->d_un.d_ptr;
        else if (ptr->d_tag == DT_STRSZ) str_table_size = ptr->d_un.d_val;
    }
    return {str_table, str_table_size};
}
