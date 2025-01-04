#include "source_file.hpp"

#include <filesystem>
#include <fstream>
#include <elf.h>
#include <link.h>

#include "plthook/plthook.h"



source_file_t::source_file_t(dll_t& dll, const fs::path& path, type_t type)
    : dll(dll), source_path(path),
        type(type == UNIT && source_path.extension().string().starts_with(".h") ? PCH : type) {}

source_file_t::source_file_t(source_file_t&& lhs) : dll(lhs.dll) {
    source_path = std::move(lhs.source_path);
    compiled_path = std::move(lhs.compiled_path);
    type = std::move(lhs.type);
    last_write_time = std::move(lhs.last_write_time);
    latest_dll = std::move(lhs.latest_dll);
    module_name = std::move(lhs.module_name);
    header_dependencies = std::move(lhs.header_dependencies);
    module_dependencies = std::move(lhs.module_dependencies);
    build_pch_includes = std::move(lhs.build_pch_includes);
    dependent_files = std::move(lhs.dependent_files);
}

// Returns true if it must be compiled.
bool source_file_t::load_dependencies(init_data_t& init_data) {
    compiled_path = (dll.output_directory / source_path);
    if (typeIsPCH())
        compiled_path += ".gch";
    else
        compiled_path.replace_extension(".o");

    fs::path dependencies_path = fs::path(compiled_path).replace_extension(".d");
    fs::path modules_path = fs::path(compiled_path).replace_extension(".dm");

    if (!fs::exists(dependencies_path) || !fs::exists(modules_path))
        // .d and/or the .md file does not exist, so create it.
        create_dependency_files(dependencies_path, modules_path);

    // Load the module dependency file, which also determines if this file is a module.
    load_module_dependencies(modules_path);
    if (!fs::exists(compiled_path)) last_write_time = {};
    else last_write_time = fs::last_write_time(compiled_path);

    // Try to get the last write time of all the headers.
    std::optional<fs::file_time_type> sources_edit_time
        = load_header_dependencies(dependencies_path, init_data);

    // If one of the sources was changed after the dependencies, or
    // one of the sources doesn't exist anymore, we have to update
    // the dependency files.
    if (!sources_edit_time || !fs::exists(dependencies_path) || fs::last_write_time(dependencies_path) < *sources_edit_time) {
        create_dependency_files(dependencies_path, modules_path);
        load_module_dependencies(modules_path);
        sources_edit_time = load_header_dependencies(dependencies_path, init_data);

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
void source_file_t::create_dependency_files(const fs::path& dependencies_path, const fs::path& modules_path) {
    dll.log_info("Creating dependencies for", source_path);
    fs::create_directories(compiled_path.parent_path());

    // TODO: replace this by just reading the #include from the files?
    std::string cmd = "clang-scan-deps -format=p1689 -- " + get_build_command() + " -MF \"" + dependencies_path.string() + "\"";
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
void source_file_t::load_module_dependencies(const fs::path& path) {
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

std::optional<fs::file_time_type> source_file_t::load_header_dependencies(const fs::path& path, init_data_t& init_data) {
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
                    header_dependencies.insert(fs::relative(s, dll.working_directory));
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


// public:
bool source_file_t::has_source_changed() {
    if (typeIsPCH())
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

std::string source_file_t::get_build_command(bool live_compile, fs::path* output_path) {
    std::ostringstream command;
    command << dll.build_command;

    // Add include directories to the build command, with possible compiled dirs.
    for (std::string_view& dir : dll.build_include_dirs) {
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
        : dll.output_directory / "tmp"
            / ("tmp" + (std::to_string(dll.temporary_files.size()) + ".so"));

    // Include the parent dir of every file.
    if (dll.include_source_parent_dir) {
        if (source_path.has_parent_path())
            command << " -I" << source_path.parent_path().string();
        else
            command << " -I.";
    }

    if (typeIsPCH())
        command << " -c -x c++-header ";
    else if (!live_compile) {
        if (type == MODULE)
            command << " --precompile ";
        else
            command << " -c ";
    }
    else {
        command << " -shared ";
        if (dll.rebuild_with_O0)
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
bool source_file_t::compile(bool live_compile) {
    fs::path output_path;
    std::string build_command = get_build_command(live_compile, &output_path);

    std::string print_command = dll.verbose ? build_command : "";
    dll.log_info("Compiling", source_path, "to", output_path, print_command);

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
        dll.temporary_files.push_back(latest_dll);
    }

    if (err != 0) {
        dll.log_info("Error compiling", compiled_path, ": ", err);
        return true;
    }

    if (type == MODULE) {
        // Create a symlink to the module.
        fs::path symlink = dll.modules_directory / (module_name + ".pcm");
        fs::remove(symlink);
        fs::create_symlink(fs::relative(output_path, dll.modules_directory), symlink);
    }

    if (live_compile)
        dll.log_info("Done!");
    return false;
}

void source_file_t::replace_functions() {
    link_map* handle = (link_map*)dlopen(latest_dll.c_str(), RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
    if (handle == nullptr) {
        dll.log_info("Error loading", latest_dll);
        return;
    }

    dll.loaded_handles.push_back(handle);
    std::string_view str_table = get_string_table(handle);

    for (size_t i = 0, l = str_table.size() - 1; i < l; ++i) {
        if (str_table[i] == '\0') {
            const char* name = &str_table[i + 1];
            void* func = dlsym(handle, name);
            // std::cout << name << std::endl;

            if (func != nullptr && strlen(name) > 3) {
                bool is_cpp = name[0] == '_' && name[1] == 'Z';
                if (is_cpp) {
                    int error = plthook_replace(dll.plthook, name, func, NULL);
                    (void)error;

                // if (error == 0)
                    // std::cout << "        " << error << std::endl;
                }
            }
        }
    }
}

std::generator<source_file_t&> source_file_t::get_header_dependencies( std::map<fs::path, source_file_t*>& map ) {
    for (const fs::path& header : header_dependencies) {
        auto it = map.find(header);
        if (it != map.end()) {
            if (this != it->second)
                co_yield *it->second;
        }
        else {
            // dll.log_error("Error in", source_path, ": header", header, "does not exist");
        }
    }
}

std::generator<source_file_t&> source_file_t::get_module_dependencies( std::map<std::string, source_file_t*>& map ) {
    for (const std::string& mod : module_dependencies) {
        auto it = map.find(mod);
        if (it != map.end()) {
            if (this != it->second)
                co_yield *it->second;
        }
        else {
            dll.log_error("Error in", source_path, ": module", mod, "does not exist");
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
