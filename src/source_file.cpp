#include "source_file.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <elf.h>
#include <link.h>
#include <string_view>
#include <system_error>

#include "plthook/plthook.h"

// Make a path lexically normal, and possibly relative to the working directory
static fs::path normalise_path(const Context& context, const fs::path& path) {
    std::error_code err;
    fs::path absolute_path = fs::canonical(path, err);
    fs::path relative_path = fs::relative(path, context.working_directory, err);
    if (relative_path.native()[0] != '.')
        return relative_path;
    else
        return absolute_path;
}

struct Parser {
    enum Return { OK, OPEN_FAILED, UNEXPECTED_END, PATH_TOO_LONG };

    SourceFile& file;
    char path[4096];
    size_t path_len = 0;

    std::ifstream f; // TODO: use a direct buffer here.
    char c;

    Parser(SourceFile& file) : file(file), f(file.source_path) {
    }

    Return parse(const Context& context) {
        if (!f.is_open())
            return OPEN_FAILED;

        c = read_char();

        empty_space: switch (c) { // anything after a newline.
            case '/': parse_comment();
            case ';': case ' ': case '\r': case '\n': c = read_char(); goto empty_space;
            case '#': goto include_i;
            case 'i': goto import_m;
            case 'm': goto module_o;
            case EOF: case '{': case '(': return OK;
            default: c = read_char(); goto token;
        }

        token: switch (c) {  // Wait until we hit a newline or a ;
            case '/': parse_comment();
            case ';': case ' ': case '\r': case '\n': c = read_char(); goto empty_space;
            case EOF: case '{': case '(': return OK;
            default: c = read_char(); goto token;
        }

        include_i: if ((c = read_char()) == 'i') goto include_n; else goto token;
        include_n: if ((c = read_char()) == 'n') goto include_c; else goto token;
        include_c: if ((c = read_char()) == 'c') goto include_l; else goto token;
        include_l: if ((c = read_char()) == 'l') goto include_u; else goto token;
        include_u: if ((c = read_char()) == 'u') goto include_d; else goto token;
        include_d: if ((c = read_char()) == 'd') goto include_e; else goto token;
        include_e: if ((c = read_char()) == 'e') goto include_spaces; else goto token;

        import_m: if ((c = read_char()) == 'm') goto import_p; else goto token;
        import_p: if ((c = read_char()) == 'p') goto import_o; else goto token;
        import_o: if ((c = read_char()) == 'o') goto import_r; else goto token;
        import_r: if ((c = read_char()) == 'r') goto import_t; else goto token;
        import_t: if ((c = read_char()) == 't') goto import_spaces; else goto token;

        module_o: if ((c = read_char()) == 'o') goto module_d; else goto token;
        module_d: if ((c = read_char()) == 'd') goto module_u; else goto token;
        module_u: if ((c = read_char()) == 'u') goto module_l; else goto token;
        module_l: if ((c = read_char()) == 'l') goto module_e; else goto token;
        module_e: if ((c = read_char()) == 'e') goto module_spaces; else goto token;

        include_spaces: switch (c = read_char()) {
            case '/': parse_comment();
            case ' ': case '\t': goto include_spaces;
            case '<': path_len = 0; goto include_read_bracket;
            case '"': path_len = 0; goto include_read_quote;
            case EOF: return UNEXPECTED_END;
            default: goto token;
        }
        include_read_bracket: switch (c = read_char()) {
            case '>':
                path[path_len] = '\0';
                file.header_dependencies.emplace(path, SourceFile::SYSTEM_HEADER);
                goto token;
            case EOF: return UNEXPECTED_END;
            default:
                if (!path_add_char(c))
                    return PATH_TOO_LONG;
                goto include_read_bracket;
        }
        include_read_quote: switch (c = read_char()) {
            case '"':
                path[path_len] = '\0';
                register_include(context);
                goto token;
            case EOF: return UNEXPECTED_END;
            default:
                if (!path_add_char(c))
                    return PATH_TOO_LONG;
                goto include_read_quote;
        }

        import_spaces: switch (c = read_char()) {
            case '/': parse_comment();
            case ' ': case '\t': case '\n': case '\r': goto import_spaces;
            case EOF: return UNEXPECTED_END;
            default:
                path[0] = c;
                path_len = 1;
                goto import_read;
        }
        import_read: switch (c = read_char()) {
            case '/': parse_comment();
            case ';': case ' ': case '\t': case '\n': case '\r':
                path[path_len] = '\0';
                file.module_dependencies.insert(path);
                c = read_char();
                goto empty_space;
            case EOF: return UNEXPECTED_END;
            default:
                if (!path_add_char(c))
                    return PATH_TOO_LONG;
                goto import_read;
        }

        module_spaces: switch (c = read_char()) {
            case '/': parse_comment();
            case ' ': case '\t': case '\n': case '\r': goto module_spaces;
            case EOF: return UNEXPECTED_END;
            default:
                path[0] = c;
                path_len = 1;
                goto module_read;
        }
        module_read: switch (c = read_char()) {
            case '/': parse_comment();
            case ';': case ' ': case '\t': case '\n': case '\r':
                path[path_len] = '\0';
                file.module_name = path;
                c = read_char();
                goto empty_space;
            case EOF: return UNEXPECTED_END;
            default:
                if (!path_add_char(c))
                    return PATH_TOO_LONG;
                goto module_read;
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
        path[path_len++] = c;
        return path_len != sizeof(path);
    }

    void register_include(const Context& context) {
        // TODO: do this all with one or zero allocations.
        fs::path path(this->path);
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
            file.header_dependencies.emplace(normalise_path(context, path), SourceFile::HEADER);
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
        if (ext == "c" || ext == "cpp")
            return {UNIT};
        else if (ext == "h" || ext == "hpp")
            return {HEADER};
        else if (ext == "cppm")
            return {MODULE};
    }
    return {};
}

void SourceFile::read_dependencies(Context& context) {
    std::error_code err;

    if (type == SYSTEM_HEADER) {
        // Check the compiled path write time.
        compiled_path = context.output_directory / "system" / source_path;
        compiled_path += ".gch";

        for (const fs::path& dir : context.system_include_dirs) {
            source_time = fs::last_write_time(dir / source_path, err);
            if (!err)
                break;
        }
    }
    else {
        // Check the compiled path write time.
        compiled_path = context.output_directory / source_path;
        compiled_path += is_header() ? ".gch" : ".o";
        source_time = fs::last_write_time(source_path, err);

        // Get the dependencies of anything but the system headers.
        Parser parser(*this);
        switch (parser.parse(context)) {
            case Parser::OK: break;
            case Parser::OPEN_FAILED:
                context.log_error("Failed to open file ", source_path);
                break;
            case Parser::UNEXPECTED_END:
                context.log_error("Parsing error in ", source_path, ", trying to continue");
                break;
            case Parser::PATH_TOO_LONG:
                context.log_error("A path or name in ", source_path, " is larger than 4096 characters");
                break;
        }
    }

    auto compiled_write_time = fs::last_write_time(compiled_path, err);
    if (!err) {
        compiled_time = compiled_write_time;
        has_changed = source_time > compiled_time;
    }
    else {
        fs::create_directories(compiled_path.parent_path());
        has_changed = true;
    }
}


bool SourceFile::has_source_changed() {
    if (is_header())
        return false;
    std::error_code err;
    fs::file_time_type new_write_time = fs::last_write_time(source_path, err);
    if (err)
        return true;
    if (!compiled_time || *compiled_time < new_write_time) {
        std::cout << source_path << " changed!\n";
        compiled_time = new_write_time;
        return true;
    }
    return false;
}

std::string SourceFile::get_build_command(const Context& context, bool live_compile, fs::path* output_path) {
    std::ostringstream command;
    command << context.build_command;

    // Add include directories to the build command, with possible compiled dirs.
    for (const std::string_view& dir : context.build_include_dirs) {
        // if (output_path != nullptr)
            // command << " -I\"" << dll.output_directory.string() << '/' << dir << '"';
        command << " -I\"" << dir << '"';
    }
    if (output_path != nullptr)
        command << build_pch_includes;

    // Get/set the output path.
    fs::path output_path_owned;
    if (output_path == nullptr)
        output_path = &output_path_owned;

    bool create_temporary_object = live_compile && output_path != nullptr && type == UNIT;
    *output_path = !create_temporary_object ? compiled_path
        : context.output_directory / "tmp"
            / ("tmp" + (std::to_string(context.temporary_files.size()) + ".so"));

    // Include the parent dir of every file.
    if (context.include_source_parent_dir) {
        if (source_path.has_parent_path())
            command << " -I" << source_path.parent_path().native();
        else
            command << " -I.";
    }

    if (is_header())
        command << " -c -x c++-header ";
    else if (!live_compile) {
        if (type == MODULE)
            command << " --precompile ";
        else
            command << " -c ";
    }
    else {
        command << " -shared ";
        if (context.rebuild_with_O0)
            command << " -O0 ";
    }

    command << " -o " << output_path->native();

    if (type == SYSTEM_HEADER)
        command << " " << compiled_path.native() << ".hpp";
    else
        command << " " << source_path.native();
    return command.str();
}

// Returns true if an error occurred.
bool SourceFile::compile(Context& context, bool live_compile) {
    fs::path output_path;
    std::string build_command = get_build_command(context, live_compile, &output_path);

    std::string print_command = context.verbose ? build_command : "";
    context.log_info("Compiling ", source_path, " to ", output_path, ", ", print_command);

    // Create a fake hpp file to contain the system header.
    if (type == SYSTEM_HEADER) {
        std::ofstream h(compiled_path.native() + ".hpp");
        h << "#pragma once\n#include <" << source_path.native() << ">\n";
    }

    // Run the command, and exit when interrupted.
    int err = system(build_command.c_str());
    compilation_failed = err != 0;
    if (live_compile)
        context.temporary_files.push_back(latest_obj);

    if (WIFSIGNALED(err) && (WTERMSIG(err) == SIGINT || WTERMSIG(err) == SIGQUIT))
        exit(1); // TODO: this is ugly, we have to shut down gracefully.

    if (compilation_failed) {
        context.log_info("Error compiling ", compiled_path, ": ", err);
        return false;
    }

    latest_obj = output_path;

    std::error_code err_code;
    compiled_time = fs::last_write_time(compiled_path, err_code);

    if (type == MODULE) {
        // Create a symlink to the module.
        fs::path symlink = context.modules_directory / (module_name + ".pcm");
        fs::remove(symlink);
        fs::create_symlink(fs::relative(output_path, context.modules_directory), symlink);
    }

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
