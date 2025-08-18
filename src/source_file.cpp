#include "source_file.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <elf.h>
#include <link.h>
#include <string_view>

#include "plthook/plthook.h"


SourceFile::SourceFile(const fs::path& path, type_t type)
    : source_path(path), type(type == UNIT && source_path.extension().string().starts_with(".h") ? PCH : type),
      dependencies_count(0) {}

SourceFile::SourceFile( SourceFile&& other ) :
    source_path(std::move(other.source_path)),
    compiled_path(std::move(other.compiled_path)),
    type(std::move(other.type)),
    last_write_time(std::move(other.last_write_time)),
    latest_dll(std::move(other.latest_dll)),
    module_name(std::move(other.module_name)),
    header_dependencies(std::move(other.header_dependencies)),
    module_dependencies(std::move(other.module_dependencies)),
    build_pch_includes(std::move(other.build_pch_includes)),
    dependent_files(std::move(other.dependent_files)),
    dependencies_count(std::move(other.dependencies_count)) {
}

// Returns true if it must be compiled.
bool SourceFile::load_dependencies(Context& context, InitData& init_data) {
    compiled_path = (context.output_directory / source_path);
    if (is_header())
        compiled_path += ".gch";
    else
        compiled_path.replace_extension(".o");

    fs::path dependencies_path = fs::path(compiled_path).replace_extension(".d");
    fs::path modules_path = fs::path(compiled_path).replace_extension(".dm");

    if (!fs::exists(dependencies_path) || !fs::exists(modules_path))
        // .d and/or the .md file does not exist, so create it.
        create_dependency_files(context, dependencies_path, modules_path);

    // Load the module dependency file, which also determines if this file is a module.
    load_module_dependencies(modules_path);
    if (!fs::exists(compiled_path)) last_write_time = {};
    else last_write_time = fs::last_write_time(compiled_path);

    // Try to get the last write time of all the headers.
    std::optional<fs::file_time_type> sources_edit_time
        = load_header_dependencies(context, dependencies_path, init_data);

    // If one of the sources was changed after the dependencies, or
    // one of the sources doesn't exist anymore, we have to update
    // the dependency files.
    if (!sources_edit_time || !fs::exists(dependencies_path) || fs::last_write_time(dependencies_path) < *sources_edit_time) {
        create_dependency_files(context ,dependencies_path, modules_path);
        load_module_dependencies(modules_path);
        sources_edit_time = load_header_dependencies(context, dependencies_path, init_data);

        // Dependencies could not be loaded, so just recompile.
        if (!sources_edit_time)
            return true;
    }

    // The sources have been changed, so recompile.
    if (!last_write_time || *last_write_time < *sources_edit_time) {
        last_write_time = *sources_edit_time;
        return true;
    }

    return false;
}

//private:
void SourceFile::create_dependency_files(Context& context, const fs::path& dependencies_path, const fs::path& modules_path) {
    context.log_info("Creating dependencies for", source_path);
    fs::create_directories(compiled_path.parent_path());

    // TODO: replace this by just reading the #include from the files?
    std::string cmd = "clang-scan-deps -format=p1689 -- " + get_build_command(context) + " -MF \"" + dependencies_path.string() + "\"";
    std::array<char, 256> buffer;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    std::ofstream module_dependencies(modules_path);
    while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr) {
        module_dependencies << buffer.data();
    }
    pclose(pipe);
}

    // Load the modules .md file.
void SourceFile::load_module_dependencies(const fs::path& path) {
    module_name.clear();
    module_dependencies.clear();
    std::ifstream f(path);
    std::string line;
    enum { NONE, PROVIDES, REQUIRES } state = NONE;
    while (std::getline(f, line)) {
        std::string_view l(line);
        l = l.substr(l.find('"') + 1);
        if (l.starts_with("provides"))
            state = PROVIDES;
        else if (l.starts_with("requires"))
            state = REQUIRES;
        else if (l.starts_with("logical-name")) {
            l = l.substr(13); // skip `logical-name"`
            l = l.substr(l.find('"') + 1);
            l = l.substr(0, l.find('"'));
            if (state == PROVIDES)
                module_name = l;
            else if (state == REQUIRES)
                module_dependencies.emplace(l);
        }
    }

    // Turn this unit in a module.
    if (!module_name.empty() && type == UNIT) {
        compiled_path.replace_extension(".pcm");
        type = MODULE;
    }
}

std::optional<fs::file_time_type> SourceFile::load_header_dependencies(Context& context, const fs::path& path, InitData& init_data) {
    header_dependencies.clear();
    fs::file_time_type sources_edit_time = fs::last_write_time(source_path);
    std::ifstream f(path);
    std::vector<char> str;
    bool is_good; char c;
    do {
        is_good = f.good();
        if (!is_good || (c = f.get()) == ' ' || c == '\n' || c == EOF) {
            // End of file, so push it.
            if (str.size() == 0) continue;
            try {
                // While initialising, check if any file has changed. We
                // use a map for efficiency.
                fs::path s = fs::canonical(std::string_view{&str[0], str.size()});
                std::unique_lock<std::mutex> lock(init_data.mutex);
                auto it = init_data.file_changes.find(s);
                if (it == init_data.file_changes.end())
                    it = init_data.file_changes.emplace_hint(it, s, fs::last_write_time(s));
                if (it->second > sources_edit_time)
                    sources_edit_time = it->second;

                bool system_header = str.size() > 4 && str[0] == '/' && str[1] == 'u' && str[2] == 's' && str[3] == 'r' && str[4] == '/';
                if (!system_header) {
                    // Is a relative path so add it to the dependencies.
                    header_dependencies.insert(fs::relative(s, context.working_directory));
                }
                else if (!s.has_extension()) {
                    header_dependencies.insert(s);
                    init_data.system_headers.insert(s); // TODO: this should include all headers
                }
            }
            catch (const fs::filesystem_error&) {
                // File does not exist anymore, which mean we
                // need to rebuild the dependencies.
                return {};
            }
            str.clear();
        }
        else if (c == '\\') {
            char d = f.get();
            if (d == ' ') str.push_back(' ');
        }
        else if (c == ':')  str.clear();
        else str.push_back(c);
    } while (is_good);

    return sources_edit_time;
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

    Return parse() {
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
                debug_print("bracket");
                file.system_header_dependencies.insert(path);
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
                debug_print("quote  ");
                register_include();
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
                debug_print("import ");
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
                debug_print("module ");
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
        return path_len != sizeof(path) - 1;
    }

    inline void debug_print(const char* identifier) {
        // std::cout << identifier << ": " << path << std::endl;
    }

    void register_include() {
        // Try to find if the relative path exists.
        file.header_dependencies.insert(path);
    }
};


void SourceFile::read_dependencies(Context& context) {
    Parser parser(*this);
    switch (parser.parse()) {
        case Parser::OK: break;
        case Parser::OPEN_FAILED:
            context.log_error("Failed to open file [", source_path, "]");
            break;
        case Parser::UNEXPECTED_END:
            context.log_error("Parsing error in [", source_path, "], trying to continue");
            break;
        case Parser::PATH_TOO_LONG:
            context.log_error("A path or name in [", source_path, "] is larger than 4096 characters");
            break;
    }
}


// public:
bool SourceFile::has_source_changed() {
    if (is_header())
        return false;
    try {
        fs::file_time_type new_write_time = fs::last_write_time(source_path);
        if (!last_write_time || *last_write_time < new_write_time) {
            std::cout << source_path << " changed!\n";
            last_write_time = new_write_time;
            return true;
        }
    }
    catch (const fs::filesystem_error&) {
        return true;
    }
    return false;

    // bool changed = false;
    // for (const fs::path& p : header_dependencies) {
    //     try {
    //         fs::file_time_type new_write_time = fs::last_write_time(p);
    //         if (new_write_time > last_edit_time) {
    //             std::cout << p << " changed!\n";
    //             last_edit_time = new_write_time;
    //             changed = true;
    //             return true;
    //         }
    //     }
    //     catch (const fs::filesystem_error&) {
    //         changed = true;
    //     }
    // }
    // return changed;
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
            command << " -I" << source_path.parent_path().string();
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

    command << " -o " + output_path->string();

    if (type == SYSTEM_PCH)
        command << " " + compiled_path.replace_extension().string();
    else
        command << " " + source_path.string();
    return command.str();
}

// Returns true if an error occurred.
bool SourceFile::compile(Context& context, bool live_compile) {
    fs::path output_path;
    std::string build_command = get_build_command(context, live_compile, &output_path);

    std::string print_command = context.verbose ? build_command : "";
    context.log_info("Compiling", source_path, "to", output_path, print_command);

    // Create a fake hpp file to contain the system header.
    if (type == SYSTEM_PCH) {
        std::ofstream h(compiled_path.replace_extension());
        h << "#pragma once\n#include <" << compiled_path.filename().string() << ">\n";
    }

    // Run the command, and exit when interrupted.
    int err = system(build_command.c_str());
    if (WIFSIGNALED(err) && (WTERMSIG(err) == SIGINT || WTERMSIG(err) == SIGQUIT))
        exit(1);

    latest_dll = output_path;

    if (live_compile) {
        context.temporary_files.push_back(latest_dll);
    }

    if (err != 0) {
        context.log_info("Error compiling", compiled_path, ": ", err);
        return true;
    }

    if (type == MODULE) {
        // Create a symlink to the module.
        fs::path symlink = context.modules_directory / (module_name + ".pcm");
        fs::remove(symlink);
        fs::create_symlink(fs::relative(output_path, context.modules_directory), symlink);
    }

    if (live_compile)
        context.log_info("Done!");
    return false;
}

void SourceFile::replace_functions(Context& context) {
    link_map* handle = (link_map*)dlopen(latest_dll.c_str(), RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
    if (handle == nullptr) {
        context.log_info("Error loading", latest_dll);
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
