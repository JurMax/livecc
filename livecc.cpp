/**
 * Host the overture shared library with live reload support.
 *
 * @file main_live_host.cpp
 * @author Jurriaan van den Berg (jurriaanvdberg@gmail.com)
 * @date 2023-04-22
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "plthook/plthook_elf.c"

#include <iostream>
#include <dlfcn.h>
#include <filesystem>
#include <vector>
#include <fstream>
#include <thread>
#include <sys/stat.h>
#include <sys/types.h>

// Sources: (order from bottom to top)
// https://stackoverflow.com/questions/2694290/returning-a-shared-library-symbol-table
// https://man7.org/linux/man-pages/man3/dl_iterate_phdr.3.html
// https://stackoverflow.com/questions/55180723/memory-access-error-when-reading-pt-dynamic-entries-of-a-elf32-on-android-6-0
// https://docs.oracle.com/cd/E19683-01/816-1386/6m7qcoblk/index.html#chapter6-42444
// https://reverseengineering.stackexchange.com/questions/1992/what-is-plt-got
// https://stackoverflow.com/questions/20486524/what-is-the-purpose-of-the-procedure-linkage-table
// https://maskray.me/blog/2021-09-19-all-about-procedure-linkage-table
// https://www.qnx.com/developers/docs/7.0.0/index.html#com.qnx.doc.neutrino.prog/topic/devel_Lazy_binding.html !

// TODO: use inotify.


namespace fs = std::filesystem;

enum build_type_t {
    LIVE,       // Build and run as a live application.
    EXECUTABLE, // Build as an standalone executable.
    SHARED      // Build as an optimized shared library.
};

struct dll_t {
    link_map* handle;
    plthook_t* plthook;

    fs::path output_file;
    fs::path output_directory;

    std::string build_command;

    std::vector<void*> loaded_handles;
    std::vector<fs::path> temporary_files;

    build_type_t build_type = LIVE;
    bool include_source_parent_dir = true;
    bool rebuild_with_O0 = true;
};

struct source_file_t {
    dll_t& dll;

    fs::path source_path;
    fs::path compiled_path;

    fs::path latest_dll;
    fs::file_time_type last_edit_time;

    std::vector<fs::path> dependency_paths;
    fs::file_time_type latest_dependency_time;

    FILE* compilation_process = nullptr;

    source_file_t( const std::string_view& path, dll_t& dll )
        : source_path(path), dll(dll) {}

    // Returns true if it must be compiled.
    bool initialise() {
        compiled_path = (dll.output_directory / source_path);
        if (dll.build_type == LIVE) compiled_path.replace_extension(".lo");
        else if (dll.build_type == EXECUTABLE) compiled_path.replace_extension(".o");
        else if (dll.build_type == SHARED) compiled_path.replace_extension(".so");

        if (fs::exists(compiled_path)) {
            last_edit_time = fs::last_write_time(compiled_path);
            return load_dependencies(true);
        }
        else {
            last_edit_time = fs::last_write_time(source_path);
            return true;
        }

        return false;
    }

    bool load_dependencies(bool initialise = false) {
        dependency_paths.clear();
        std::ifstream f(fs::path(compiled_path).replace_extension(".d"));
        std::string s;

        bool changed = false;
        if (f.is_open()) {
            while (std::getline(f, s, ' ')) {
                if (s[0] == '\\' || s.find(':') != std::string::npos)
                    continue;

                if (s[0] == '/') {
                    // TODO: Create a map of the filetimes, so things don't get checked
                    // more than once during startup.

                    // Check if any library files have changed.
                    if (initialise)
                        changed = changed || check_if_changed(s);
                }
                else {
                    // Is a relative path so add it to the dependencies.
                    dependency_paths.push_back(s);
                }
            }
        }

        // Add the source file if no .d file was found.
        if (dependency_paths.empty())
            dependency_paths.push_back(source_path);

        if (initialise)
            return was_changed() || changed;
        return false;
    }

    bool check_if_changed(const fs::path& p) {
        try {
            fs::file_time_type new_write_time = fs::last_write_time(p);
            if (new_write_time > last_edit_time) {
                // std::cout << "Changed! " << p << std::endl;
                last_edit_time = new_write_time;
                return true;
            }
        }
        catch (const fs::filesystem_error&) {
            return true;
        }

        return false;
    }

    bool was_changed() {
        bool changed = false;
        for (const fs::path& p : dependency_paths)
            changed = check_if_changed(p) || changed;
        return changed;
    }

    void start_compile_process(bool initialise = false) {
        // Make sure the previous compilation process is done.
        end_compile_process();

        fs::path output_path;

        if (initialise) {
            output_path = compiled_path;
            std::cout << "Compiling " << source_path << std::endl;
        }
        else {
            output_path = dll.output_directory / "tmp"
                / ("tmp" + (std::to_string(dll.temporary_files.size()) + ".so"));
            std::cout << "Compiling " << source_path << " to " << output_path << std::endl;
        }

        fs::create_directories(output_path.parent_path());

        std::string command = dll.build_command;
        if (dll.include_source_parent_dir) {
            if (source_path.has_parent_path())
                command += " -I" + source_path.parent_path().string();
            else
                command += " -I.";
        }
        command += " -o " + output_path.string();
        command += " " + source_path.string();

        if (initialise)
            command += " -c ";
        if (!initialise && !dll.rebuild_with_O0)
            command += " -O0 ";

        // std::cout << "Running: " << command << std::endl;
        compilation_process = popen(command.c_str(), "r");
        latest_dll = output_path;
        if (!initialise) {
            dll.temporary_files.push_back(latest_dll);
        }
    }

    // Returns true when there is an error.
    bool end_compile_process() {
        if (compilation_process == nullptr)
            return false;

        int error = pclose(compilation_process);
        compilation_process = nullptr;

        if (error != 0) {
            std::cout << "Error compiling " << compiled_path << ": " << error << std::endl;
            return true;
        }
        else {
            load_dependencies();
            return false;
        }
    }

    void replaceFunctions() {
        link_map* handle = (link_map*)dlopen(latest_dll.c_str(), RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
        if (handle == nullptr) {
            std::cout << "Error loading " << latest_dll << std::endl;
            return;
        }

        dll.loaded_handles.push_back(handle);

        // Find the string table.
        size_t str_table_size = 0;
        const char* str_table;
        for (auto ptr = handle->l_ld; ptr->d_tag; ++ptr) {
            if (ptr->d_tag == DT_STRTAB) str_table = (const char*)ptr->d_un.d_ptr;
            else if (ptr->d_tag == DT_STRSZ) str_table_size = ptr->d_un.d_val;
        }

        for (size_t i = 0; i < str_table_size - 1; ++i) {
            if (str_table[i] == '\0') {
                const char* name = &str_table[i + 1];
                void* func = dlsym(handle, name);
                // std::cout << name << std::endl;

                if (func != nullptr && strlen(name) > 3) {
                    bool is_cpp = name[0] == '_' && name[1] == 'Z';
                    if (is_cpp) {
                        int error = plthook_replace(dll.plthook, name, func, NULL);

                    // if (error == 0)
                        // std::cout << "        " << error << std::endl;
                    }
                }
            }
        }
    }
};



#define CHK_PH(func) do { \
    if (func != 0) { \
        fprintf(stderr, "%s error: %s\n", #func, plthook_error()); \
        exit(1); \
    } \
} while (0)


typedef void dll_callback_func_t(void);
typedef int set_callback_func_t(dll_callback_func_t*);

struct live_cc_t {
    dll_t dll;

    size_t path_index = 0;
    std::vector<source_file_t> files;

    void parse_arguments(int argn, char** argv) {
        dll.output_file = "build/a.out";
        std::ostringstream build_command;
        build_command << "gcc ";

        bool is_input = true;
        bool is_output = false;
        for (int i = 1; i < argn; ++i) {
            std::string_view arg(argv[i]);

            if (arg[0] == '-') {
                // Get the output file.
                if (arg.length() >= 2 && arg[1] == 'o') {
                    if (arg.length() == 2)
                        is_output = true;
                    else
                        dll.output_file = arg.substr(2);
                    continue;
                }
                else if (arg == "--executable") {
                    dll.build_type = EXECUTABLE;
                    continue;
                }
                else if (arg == "--shared") {
                    dll.build_type = SHARED;
                    continue;
                }
                else if (arg == "--no-rebuild-with-O0") {
                    dll.rebuild_with_O0 = false;
                    continue;
                }

                build_command << ' ' << arg;

                // The next argument is part of this flag, so it's not a file.
                if (arg.length() == 2)
                    is_input = false;

            }
            else if (is_output) {
                dll.output_file = arg;
                is_output = false;
                is_input = true;
            }
            else if (is_input) {
                files.emplace_back(arg, dll);
            }
            else {
                build_command << ' ' << arg;
                is_input = true;
            }
        }

        if (dll.build_type == LIVE || dll.build_type == SHARED)
            build_command << " -shared";
        if (dll.build_type == LIVE)
            build_command << " -fPIC -fno-inline -fno-ipa-sra";
        dll.build_command = build_command.str();
        dll.output_directory = dll.output_file.parent_path();
    }

    bool compile_and_link() {
        // Make sure all the files are compiled.
        std::vector<source_file_t> to_compile;
        for (source_file_t& file : files) {
            if (file.initialise())
                to_compile.push_back(file);
        }

        int processor_count = std::max(1, (int)std::thread::hardware_concurrency() - 1);

        // Compile all the programs
        bool success = true;
        for (int started_i = 0, i = -processor_count; i < (int)to_compile.size(); ++i) {
            if (i >= 0 && to_compile[i].end_compile_process())
                success = false;
            if (success && started_i < (int)to_compile.size())
                to_compile[started_i++].start_compile_process(true);
        }

        if (success == false) {
            return false;
        }

        // link all the files into one shared library.
        std::ostringstream link_command;
        link_command << dll.build_command;
        link_command << " -o " << dll.output_file;
        for (source_file_t& file : files)
            link_command << ' ' << file.compiled_path;
        std::cout << "Linking sources together..." << std::endl;
        if (int err = system(link_command.str().c_str())) {
            std::cout << "Error linking to " << dll.output_file << ": " << err << std::endl;
            return false;
        }
        else {
            return true;
        }
    }

    void start( dll_callback_func_t* callback_func ) {
        // Open the created shared library.
        dll.handle = (link_map *)dlopen(dll.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (dll.handle == nullptr)
            std::cout << "Error loading application: " << dlerror() << std::endl;

        CHK_PH(plthook_open_by_handle(&dll.plthook, dll.handle));

        set_callback_func_t* set_callback = (set_callback_func_t*)dlsym(dll.handle, "setDLLCallback");
        if (set_callback == nullptr)
            std::cout << "No setDLLCallback() found, so we can't check for file changes!" << std::endl;
        else
            (*set_callback)(callback_func);

        // Run the main function till we're done.
        typedef int mainFunc(int, char**);
        mainFunc* main_func = (mainFunc*)dlsym(dll.handle, "main");
        if (main_func == nullptr)
            std::cout << "No main function found, so we can't start the application!" << std::endl;
        else
            (*main_func)(0, nullptr);

        std::cout << "Ending live reload session" << std::endl;
        close();

        plthook_close(dll.plthook);
        dlclose(dll.handle);
    }

    void close() {
        // Close all the dlls.
        while (!dll.loaded_handles.empty()) {
            dlclose(dll.loaded_handles.back());
            dll.loaded_handles.pop_back();
        }

        // Delete the temporary files.
        while (!dll.temporary_files.empty()) {
            fs::remove(dll.temporary_files.back());
            dll.temporary_files.pop_back();
        }

        files.clear();
    }

    void update() {
        path_index = (path_index + 1) % files.size();

        source_file_t& file = files[path_index];
        if (file.was_changed()) {
            file.start_compile_process();
            file.end_compile_process();
            file.replaceFunctions();
        }
    }
};

static live_cc_t live_cc;

void callback() {
    live_cc.update();
}


int main(int argn, char** argv) {
    // std::cout << "Starting live reload session" << std::endl;

    live_cc.parse_arguments(argn, argv);
    if (live_cc.compile_and_link() && live_cc.dll.build_type == LIVE)
        live_cc.start(callback);

    return 0;
}

// TODO: our own command line arguments:
//  -h/--help
// PCH support
//     Automatic PCH support (check which headers are used most, how many percent of files use them).
// TODO: move static stuff where possible

