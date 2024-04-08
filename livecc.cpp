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

#include <algorithm>
#include <iostream>
#include <dlfcn.h>
#include <filesystem>
#include <string_view>
#include <vector>
#include <fstream>
#include <thread>
#include <map>
#include <unordered_map>
#include <set>

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
    LIVE,         // Build and run as a live application.
    SHARED,       // Build as an optimized shared library.
    EXECUTABLE,   // Build as an standalone executable.
    DEPENDENCIES, // Build only the .d files.
    CLEAN         // Delete all the build files.
};

struct dll_t {
    fs::path working_directory;
    fs::path output_file;
    fs::path output_directory;

    std::string build_command;
    build_type_t build_type = LIVE;
    bool include_source_parent_dir = true;
    bool rebuild_with_O0 = true;

    // Runtime.
    link_map* handle;
    plthook_t* plthook;
    std::vector<void*> loaded_handles;
    std::vector<fs::path> temporary_files;

    // Reusable array.
    std::vector<char> string_builder;
};

struct init_data_t {
    // Store the library header file file times here, so we
    // don't have to keep checking them for changes.
    std::unordered_map<fs::path, fs::file_time_type> library_changes;
};

struct source_file_t {
    dll_t& dll;

    fs::path source_path;
    fs::path compiled_path;
    bool is_pch_header = false;

    fs::file_time_type last_edit_time;
    std::vector<fs::path> dependency_paths;

    fs::path latest_dll;
    FILE* compilation_process = nullptr;

    source_file_t(dll_t& dll, const std::string_view& path, bool is_pch_header = false)
        : dll(dll), source_path(path), is_pch_header(is_pch_header) {}

    // Returns true if it must be compiled.
    void set_paths() {
        compiled_path = (dll.output_directory / source_path);
        std::string extension;
        if (dll.build_type == LIVE) extension = ".lo";
        else if (dll.build_type == SHARED) extension = ".so";
        else /* if (dll.build_type == EXECUTABLE) */ extension = ".o";

        if (is_pch_header)
            compiled_path.replace_extension(extension + ".h.gch");
        else
            compiled_path.replace_extension(extension);
    }

    bool initialise(init_data_t* init_data = nullptr) {
        set_paths();

        if (init_data != nullptr) {
            if (fs::exists(compiled_path)) {
                last_edit_time = fs::last_write_time(compiled_path);
                return load_dependencies(init_data);
            }
            else {
                last_edit_time = fs::last_write_time(source_path);
                return true;
            }
        }

        return false;
    }

    bool load_dependencies(init_data_t* init_data = nullptr) {
        dependency_paths.clear();
        std::ifstream f(fs::path(compiled_path).replace_extension(".d"));

        bool changed = false;
        if (f.is_open()) {
            std::vector<char>& str = dll.string_builder;
            bool is_good;
            do {
                is_good = f.good();
                char c;
                if (!is_good || (c = f.get()) == ' ' || c == '\n' || c == EOF) {
                    // End of file, so push it.
                    if (str.size() == 0)
                        continue;

                    fs::path s = fs::canonical(std::string_view{&str[0], str.size()});

                    // While initialising, check if any file has changed. We
                    // use a map for efficiency.
                    if (init_data != nullptr) {
                        auto it = init_data->library_changes.find(s);
                        if (it == init_data->library_changes.end())
                            it = init_data->library_changes.emplace_hint(it, s, fs::last_write_time(s));

                        if (it->second > last_edit_time) {
                            last_edit_time = it->second;
                            changed = true;
                        }
                    }

                    if (str[0] != '/') {
                        // Is a relative path so add it to the dependencies.
                        dependency_paths.emplace_back(fs::relative(s, dll.working_directory));
                    }

                    str.clear();
                }
                else if (c == '\\') {
                    char d = f.get();
                    if (d == ' ')
                        str.push_back(' ');
                }
                else if (c == ':')
                    str.clear();
                else
                    str.push_back(c);
            } while (is_good);
        }

        // Add the source file if no .d file was found.
        if (dependency_paths.empty())
            dependency_paths.emplace_back(source_path);

        return changed;
    }

    bool was_changed() {
        bool changed = false;
        for (const fs::path& p : dependency_paths) {
            try {
                fs::file_time_type new_write_time = fs::last_write_time(p);
                if (new_write_time > last_edit_time) {
                    std::cout << p << " changed!\n";
                    last_edit_time = new_write_time;
                    changed = true;
                    return true;
                }
            }
            catch (const fs::filesystem_error&) {
                changed = true;
            }
        }
        return changed;
    }

    void start_compile_process(bool initialise = false) {
        // Make sure the previous compilation process is done.
        end_compile_process();

        bool create_temporary_object = !initialise && !is_pch_header;
        fs::path output_path = !create_temporary_object ? compiled_path
            : dll.output_directory / "tmp"
                / ("tmp" + (std::to_string(dll.temporary_files.size()) + ".so"));

        std::cout << "Compiling " << source_path << " to " << output_path << std::endl;
        fs::create_directories(output_path.parent_path());

        std::string command = dll.build_command;
        if (dll.include_source_parent_dir) {
            if (source_path.has_parent_path())
                command += " -I" + source_path.parent_path().string();
            else
                command += " -I.";
        }

        if (is_pch_header)
            command += " -c -x c++-header ";
        else if (initialise)
            command += " -c ";
        else if (dll.rebuild_with_O0)
            command += " -O0 ";

        command += " -o " + output_path.string();
        command += " " + source_path.string();

        // Create a fake header file so we can include the pch later.
        if (is_pch_header) {
            fs::copy(source_path, fs::path(compiled_path).replace_extension(),
                fs::copy_options::overwrite_existing);
        }

        compilation_process = popen(command.c_str(), "r");
        latest_dll = output_path;
        if (!initialise) {
            dll.temporary_files.push_back(latest_dll);
        }
    }

    // Returns true when there is an error.
    bool end_compile_process(init_data_t* init_data = nullptr) {
        if (compilation_process == nullptr)
            return false;

        int error = pclose(compilation_process);
        compilation_process = nullptr;

        if (error != 0) {
            std::cout << "Error compiling " << compiled_path << ": " << error << std::endl;
            return true;
        }
        else {
            // A new .d file has been created, so load its dependencies again.
            load_dependencies(init_data);
            return false;
        }
    }

    void replace_functions() {
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
        dll.working_directory = fs::current_path();
        dll.output_file = "build/a.out";
        std::ostringstream build_command;
        build_command << "gcc ";

        enum { INPUT, OUTPUT, PCH, FLAG } next_arg_type = INPUT;

        for (int i = 1; i < argn; ++i) {
            std::string_view arg(argv[i]);

            if (arg[0] == '-') {
                if (arg.starts_with("-o")) {
                    // Get the output file.
                    if (arg.length() == 2)
                        next_arg_type = OUTPUT;
                    else
                        dll.output_file = arg.substr(2);
                }
                else if (arg.starts_with("--pch")) {
                    if (arg.length() == 5)
                        next_arg_type = PCH;
                    else
                        files.emplace_back(dll, arg, true);
                }
                else if (arg == "--executable")
                    dll.build_type = EXECUTABLE;
                else if (arg == "--shared")
                    dll.build_type = SHARED;
                else if (arg == "--no-rebuild-with-O0")
                    dll.rebuild_with_O0 = false;
                else if (arg == "--clean")
                    dll.build_type = CLEAN;
                else {
                    build_command << ' ' << arg;

                    // The next argument is part of this flag, so it's not a file.
                    // TODO: handle all the arguments in which you specify an option.
                    // like: -MD, --param, etc.
                    if (arg.length() == 2
                        || (arg.starts_with("-include") && arg.size() == 8))
                        next_arg_type = FLAG;
                }
            }
            else {
                if (next_arg_type == INPUT)
                    files.emplace_back(dll, arg);
                else if (next_arg_type == PCH)
                    files.emplace_back(dll, arg, true);
                else if (next_arg_type == OUTPUT)
                    dll.output_file = arg;
                else
                    build_command << ' ' << arg;
                next_arg_type = INPUT;
            }
        }

        if (dll.build_type == LIVE || dll.build_type == SHARED)
            build_command << " -shared -fPIC";
        if (dll.build_type == LIVE)
            build_command << " -fno-inline -fno-ipa-sra";
        build_command << " -MD -Winvalid-pch";

        dll.output_directory = dll.output_file.parent_path();
        dll.build_command = build_command.str();
    }

    bool compile_files(const std::vector<source_file_t*>& to_compile, init_data_t* init_data) {
        bool success = true;
        if (to_compile.size() > 0) {
            int processor_count = std::max(1, (int)std::thread::hardware_concurrency() - 1);
            for (int started_i = 0, i = -processor_count; i < (int)to_compile.size(); ++i) {
                if (i >= 0 && to_compile[i]->end_compile_process(init_data))
                    success = false;
                if (success && started_i < (int)to_compile.size())
                    to_compile[started_i++]->start_compile_process(true);
            }
        }

        return success;
    }

    bool compile_and_link() {
        // Make sure all the files are compiled.
        bool did_compilation = false;
        {
            std::vector<source_file_t*> headers_to_compile;
            std::vector<source_file_t*> sources_to_compile;
            init_data_t init_data;

            for (source_file_t& file : files) {
                if (file.initialise(&init_data)) {
                    did_compilation = true;
                    (file.is_pch_header ? headers_to_compile : sources_to_compile)
                        .push_back(&file);
                }
            }

            // Compile all the pch headers.
            if (!compile_files(headers_to_compile, &init_data))
                return false;

            // Add the PCH files to the build command. TODO: do this more efficiently.
            for (source_file_t& file : files)
                if (file.is_pch_header)
                    dll.build_command += " -include " + fs::path(file.compiled_path).replace_extension().string();

            // Build all the source files.
            if (!compile_files(sources_to_compile, &init_data))
                return false;
        }

        bool link = did_compilation || !fs::exists(dll.output_file);

        // link all the files into one shared library.
        if (link) {
            std::ostringstream link_command;
            link_command << dll.build_command;
            link_command << " -o " << dll.output_file;
            for (source_file_t& file : files)
                if (!file.is_pch_header)
                    link_command << ' ' << file.compiled_path;

            std::cout << "Linking sources together..." << std::endl;
            if (int err = system(link_command.str().c_str())) {
                std::cout << "Error linking to " << dll.output_file << ": " << err << std::endl;
                return false;
            }
        }

        return true;
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
            file.replace_functions();
        }
    }

    void clean() {
        std::cout << "Cleaning all files" << std::endl;
        constexpr const char* extensions[] = { ".o", ".lo", "so" };
        for (source_file_t& file : files) {
            file.initialise();
            if (file.is_pch_header) {
                std::string path = file.compiled_path
                    .replace_extension()/*.gch*/.replace_extension()/*.h*/
                    .replace_extension()/*.o*/.string();

                for (const char* extension : extensions) {
                    fs::remove(path + extension + ".h");
                    fs::remove(path + extension + ".h.gch");
                    fs::remove(path + extension + ".h.d");
                }
            }
            else {
                for (const char* extension : extensions)
                    fs::remove(file.compiled_path.replace_extension(extension));
                fs::remove(file.compiled_path.replace_extension(".d"));
            }
        }
        fs::remove(dll.output_file);
    }
};

static live_cc_t live_cc;

void callback() {
    live_cc.update();
}


int main(int argn, char** argv) {
    // std::cout << "Starting live reload session" << std::endl;

    live_cc.parse_arguments(argn, argv);
    if (live_cc.dll.build_type  == CLEAN)
        live_cc.clean();
    else if (live_cc.compile_and_link() && live_cc.dll.build_type == LIVE)
        live_cc.start(callback);

    return 0;
}

// TODO: our own command line arguments:
//  -h/--help
// TODO: move static stuff where possible

// Glob support.
