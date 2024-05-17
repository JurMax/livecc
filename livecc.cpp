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
#include <initializer_list>
#include <iostream>
#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <thread>
#include <iomanip>
#include <map>
#include <format>
#include <unordered_map>
#include <set>
#include <csignal>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "thread_pool.hpp"

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
    STANDALONE,   // Build as an standalone executable.
};

struct dll_t {
    fs::path working_directory;
    fs::path output_file;
    fs::path output_directory;

    fs::path modules_directory;

    build_type_t build_type = LIVE;
    std::string build_command;
    std::vector<std::string_view> build_include_dirs;

    bool include_source_parent_dir = true;
    bool rebuild_with_O0 = false;

    std::string link_arguments;

    // The amount of files to compile in parallel.
    int job_count = 0;

    // Runtime.
    link_map* handle;
    plthook_t* plthook;
    std::vector<void*> loaded_handles;
    std::vector<fs::path> temporary_files;

private:
    std::mutex print_mutex;
    std::string task_name;
    int bar_task_current;
    int bar_task_total;
    int term_width;

public:
    dll_t() {
        struct winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        term_width = w.ws_col;
    }

    template<typename ...Args>
    void log_info(const Args&... args) {
        std::unique_lock<std::mutex> lock(print_mutex);
        std::ostringstream ss;
        auto _ = { (ss << args << ' ', 0)... };
        std::string to_print = ss.str();
        std::cout << to_print;
        for (int i = to_print.size() + 1; i < term_width; ++i)
            std::cout << ' ';
        std::cout << std::endl;

        if (!task_name.empty()) {
            print_bar();
        }
    }

    template<typename ...Args>
    void log_error(const Args&... args) { log_info(args...); }

    void log_set_task(const std::string_view& task, int task_total) {
        task_name = task;
        bar_task_total = task_total;
        bar_task_current = 0;
    }
    void log_clear_task() {
        task_name.clear();
    }
    void log_step_task() {
        std::unique_lock<std::mutex> lock(print_mutex);
        ++bar_task_current;
        print_bar();
    }
private:
    void print_bar() {
        std::cout << task_name << " [";
        int length = term_width - task_name.length() - 2 - 7;
        int progress = bar_task_current * length / bar_task_total;
        int i = 0;
        for (; i < progress; ++i) std::cout << '=';
        if (i < length) std::cout << '>';
        for (++i; i < length; ++i) std::cout << ' ';
        std::cout << std::format("] {:>3}%\r", bar_task_current * 100 / bar_task_total);
        std::cout.flush();
    }
};

struct init_data_t {
    // Store the library header file file times here, so we
    // don't have to keep checking them for changes.
    std::unordered_map<fs::path, fs::file_time_type> file_changes;
    std::mutex mutex;
    std::set<fs::path> system_headers;
};


struct source_file_t {
    enum type_t {
        UNIT,
        PCH,
        SYSTEM_PCH,
        MODULE,
        // HEADER_UNIT,
    };

    dll_t& dll;

    fs::path source_path;
    fs::path compiled_path;

    type_t type;
    inline bool typeIsPCH() const { return type == PCH || type == SYSTEM_PCH; }

    std::optional<fs::file_time_type> last_write_time;

    fs::path latest_dll;

    // TODO: implement this. Also add support for header units. Used header units
    // should also be added to the source files with the header unit type
    std::string module_name; // if type == MODULE.

    std::set<fs::path> header_dependencies;
    std::set<std::string> module_dependencies;
    std::string build_pch_includes; // pch includes to add to the build command.

    // RUNTIME:
    // Files that depend on this module. These get added to the queue
    // once this file is done with compiling, and they have no other
    // dependencies left.
    std::vector<source_file_t*> dependent_files;

    std::atomic<int> compiled_dependencies; // When this is equal to dependencies_count, we can compile.
    int dependencies_count = 0;

    source_file_t(dll_t& dll, const fs::path& path, type_t type = UNIT)
        : dll(dll), source_path(path),
          type(type == UNIT && source_path.extension().string().starts_with(".h") ? PCH : type) {}

    source_file_t(source_file_t&& lhs) : dll(lhs.dll) {
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
    bool load_dependencies(init_data_t& init_data) {
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
        if (!sources_edit_time || fs::last_write_time(dependencies_path) < *sources_edit_time) {
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

private:
    void create_dependency_files(const fs::path& dependencies_path, const fs::path& modules_path) {
        dll.log_info("Creating dependencies for", source_path);
        fs::create_directories(compiled_path.parent_path());

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
    void load_module_dependencies(const fs::path& path) {
        module_name.clear();
        module_dependencies.clear();
        std::ifstream f(path);
        std::string line;
        enum { NONE, PROVIDES, REQUIRES } state;
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

    std::optional<fs::file_time_type> load_header_dependencies(const fs::path& path, init_data_t& init_data) {
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
                        init_data.system_headers.insert(s);
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


public:
    bool has_source_changed() {
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

    std::string get_build_command(bool live_compile = false, fs::path* output_path = nullptr) {
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
    bool compile(bool live_compile = false) {
        fs::path output_path;
        std::string build_command = get_build_command(live_compile, &output_path);

        dll.log_info("Compiling", source_path, "to", output_path);

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

        return false;
    }

    void replace_functions() {
        link_map* handle = (link_map*)dlopen(latest_dll.c_str(), RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
        if (handle == nullptr) {
            dll.log_info("Error loading", latest_dll);
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
        build_command << "/usr/bin/clang ";

        enum { INPUT, OUTPUT, PCH, FLAG } next_arg_type = INPUT;

        for (int i = 1; i < argn; ++i) {
            std::string_view arg(argv[i]);

            if (arg[0] == '-') {
                if (arg[1] == 'o') {
                    // Get the output file.
                    if (arg.length() == 2)
                        next_arg_type = OUTPUT;
                    else
                        dll.output_file = arg.substr(2);
                }
                else if (arg[1] == 'j') {
                    // Set the number of parallel threads to use.
                    if (arg.length() == 2)
                        dll.job_count = std::stoi(argv[++i]);
                    else
                        dll.job_count = std::stoi(argv[i] + 2);
                }
                else if (arg[1] == 'l' || arg[1] == 'L') {
                    dll.link_arguments += " ";
                    dll.link_arguments += arg;
                }
                else if (arg[1] == 'I') {
                    std::string_view dir = std::string_view(arg).substr(2);
                    if (dir[0] == '"' && dir[dir.length() - 1] == '"')
                        dir = dir.substr(1, dir.length() - 2);
                    dll.build_include_dirs.push_back(dir);
                }
                else if (arg.starts_with("--pch")) {
                    if (arg.length() == 5)
                        next_arg_type = PCH;
                    else
                        files.emplace_back(dll, arg, source_file_t::PCH);
                }
                else if (arg == "--standalone")
                    dll.build_type = STANDALONE;
                else if (arg == "--shared")
                    dll.build_type = SHARED;
                else if (arg == "--no-rebuild-with-O0")
                    dll.rebuild_with_O0 = false;
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
                    files.emplace_back(dll, arg, source_file_t::PCH);
                else if (next_arg_type == OUTPUT)
                    dll.output_file = arg;
                else
                    build_command << ' ' << arg;
                next_arg_type = INPUT;
            }
        }

        // Set the output directory.
        dll.output_directory = dll.output_file.parent_path();
        switch (dll.build_type) {
            case LIVE:       dll.output_directory /= "live"; break;
            case SHARED:     dll.output_directory /= "shared"; break;
            case STANDALONE: dll.output_directory /= "standalone"; break;
        }

        // Rename the output file if no extension has been given.
        if (!dll.output_file.has_extension()) {
            std::string new_filename = dll.output_file.filename().string();
            switch (dll.build_type) {
                case LIVE:   new_filename = "lib" + new_filename + "_live.a"; break;
                case SHARED: new_filename = "lib" + new_filename + ".a"; break;
                case STANDALONE: break;
            }
            dll.output_file = dll.output_file.parent_path() / new_filename;
        }

        // Create the temporary and the modules directories.
        dll.modules_directory = dll.output_directory / "modules";
        build_command << " -fprebuilt-module-path=" << dll.modules_directory;
        fs::create_directories(dll.modules_directory);
        fs::create_directories(dll.output_directory / "tmp");
        fs::create_directories(dll.output_directory / "system");

        if (dll.build_type == LIVE || dll.build_type == SHARED) {
            build_command << " -fPIC";
            dll.link_arguments += " -shared";
        }
        if (dll.build_type == LIVE)
            build_command << " -fno-inline";// -fno-ipa-sra";
        // -fno-ipa-sra disables removal of unused parameters, as this breaks code recompiling for functions with unused arguments for some reason.
        build_command << " -MD -Winvalid-pch";

        dll.build_command = build_command.str();
    }

    bool compile_files(std::string name, const std::vector<source_file_t*>& to_compile) {
        if (to_compile.size() > 0) {
            dll.log_set_task("COMPILING " + name, to_compile.size());
            ThreadPool pool(dll.job_count);
            for (source_file_t* f : to_compile)
                pool.enqueue([this, f] {
                    bool error = f->compile();
                    dll.log_step_task();
                    return error;
                });
            pool.join();
            dll.log_clear_task();
            return !pool.got_error;
        }
        return true;
    }

    void add_module_dependencies(std::set<source_file_t*>& to_compile, std::set<source_file_t*>& modules_to_compile) {
        // Find all the files we need to compile because of the changed modules.
        std::stack<source_file_t*> check_stack;
        for (source_file_t* f : modules_to_compile) {
            check_stack.emplace(f);
        }

        // TODO: find a way to check if the exports of a module have changed,
        // instead of recompiling every time anything in it has changed. This
        // SHOULD be possible, this would make modules so much better. If not,
        // then using modules might not be worth it........

        while (!check_stack.empty()) {
            source_file_t* f = check_stack.top();
            check_stack.pop();

            // Iterate over all the file that depend on this file,
            // and add them to the to_compile files.
            for (source_file_t* d : f->dependent_files) {
                auto it = to_compile.find(d);
                if (it == to_compile.end()) {
                    to_compile.emplace_hint(it, d);
                    check_stack.push(d);
                }
            }
        }
    }

    bool compile_files(std::set<source_file_t*>& to_compile) {
        dll.log_set_task("COMPILING", to_compile.size());
        ThreadPool pool(dll.job_count);

        // Add all files that have no dependencies to the compile queue.
        // As these compile they will add all the other files too.
        for (source_file_t& f : files) {
            if (f.dependencies_count == 0) {
                add_to_compile_queue(pool, to_compile, &f);
            }
        }

        pool.join();
        dll.log_clear_task();
        return true;
    }

private:
    void mark_compiled(ThreadPool& pool, const std::set<source_file_t*>& to_compile, source_file_t* f) {
        for (source_file_t* d : f->dependent_files) {
            if (++d->compiled_dependencies == d->dependencies_count) {
                add_to_compile_queue(pool, to_compile, d);
            }
        }
    }

    void add_to_compile_queue(ThreadPool& pool,
                              const std::set<source_file_t*>& to_compile, source_file_t* f) {
        if (to_compile.contains(f)) {
            pool.enqueue([this, &pool, &to_compile, f] {
                bool error = f->compile();
                if (!error)
                    mark_compiled(pool, to_compile, f);
                dll.log_step_task();
                return error;
            });
        }
        else {
            // We don't need to compile this file, so add its dependencies to the queue.
            mark_compiled(pool, to_compile, f);
        }
    }

    bool create_dependency_tree() {
        // module name -> source file
        std::map<std::string, source_file_t*> module_map;
        std::map<fs::path, source_file_t*> header_map;

        for (source_file_t& f : files) {
            if (f.type == source_file_t::MODULE) {
                auto it = module_map.find(f.module_name);
                if (it == module_map.end())
                    module_map.emplace_hint(it, f.module_name, &f);
                else {
                    dll.log_error("There are multiple implementations for module ", f.module_name,
                        "(", it->second->source_path, "and", f.source_path, ")");
                    return false;
                }
            }
            else if (f.typeIsPCH()) {
                header_map.emplace(f.source_path, &f);
            }
        }

        // Fill the dependent_files of each module, e.g.
        // the files that depend on that module.
        for (source_file_t& f : files) {
            for (const fs::path& header : f.header_dependencies) {
                auto it = header_map.find(header);
                if (it != header_map.end() && &f != it->second) {
                    it->second->dependent_files.push_back(&f);
                    ++f.dependencies_count;
                    if (f.type == source_file_t::SYSTEM_PCH)
                        f.build_pch_includes += " -include \"" + it->second->compiled_path.replace_extension().string() + '"';
                    f.build_pch_includes += " -include-pch \"" + it->second->compiled_path.string() + '"';
                }
            }
            for (const std::string& module : f.module_dependencies) {
                auto it = module_map.find(module);
                if (it == module_map.end()) {
                    dll.log_error("Error in", f.source_path, ": module", module, "does not exist");
                    return false;
                }
                it->second->dependent_files.push_back(&f);
                ++f.dependencies_count;
            }
        }
        return true;
    }


public:

    bool compile_and_link() {
        // Make sure all the files are compiled.
        bool did_compilation = false;
        {
            std::set<source_file_t*> modules_to_compile;
            std::set<source_file_t*> files_to_compile;

            {
                std::vector<size_t> modules_to_compile_i;
                std::vector<size_t> files_to_compile_i;

                init_data_t init_data;
                dll.log_set_task("LOADING DEPENDENCIES", files.size());
                ThreadPool pool(dll.job_count);
                for (size_t i = 0; i < files.size(); ++i)
                    pool.enqueue([&, i] () {
                        source_file_t& file = files[i];
                        if (file.load_dependencies(init_data)) {
                            std::unique_lock<std::mutex> lock(init_data.mutex);
                            files_to_compile_i.push_back(i);
                            if (file.type == source_file_t::MODULE)
                                modules_to_compile_i.push_back(i);
                        }
                        dll.log_step_task();
                        return false;
                    });
                pool.join();
                dll.log_clear_task();

                // Create system header compilation units and mark them for recompilation if necessary.
                for (const fs::path& header_path : init_data.system_headers) {
                    source_file_t& f = files.emplace_back(dll, header_path, source_file_t::SYSTEM_PCH);
                    f.compiled_path = dll.output_directory / "system" / (header_path.filename().string() + ".gch");
                    if (!fs::exists(f.compiled_path) || fs::last_write_time(f.compiled_path) < init_data.file_changes[header_path])
                        files_to_compile_i.push_back(files.size() - 1);
                }

                for (size_t i : modules_to_compile_i) modules_to_compile.insert(&files[i]);
                for (size_t i : files_to_compile_i) files_to_compile.insert(&files[i]);
            }

            if (!create_dependency_tree())
                return false;

            if (!modules_to_compile.empty())
                add_module_dependencies(files_to_compile, modules_to_compile);

            did_compilation = !files_to_compile.empty();

            // Compile all the modules headers.
            if (!compile_files(files_to_compile))
                return false;
        }

        bool link = did_compilation || !fs::exists(dll.output_file);

        // link all the files into one shared library.
        if (link) {
            std::ostringstream link_command;
            link_command << dll.build_command;
            link_command << dll.link_arguments;
            link_command << " -o " << dll.output_file;
            for (source_file_t& file : files)
                if (!file.typeIsPCH())
                    link_command << ' ' << file.compiled_path;

            dll.log_info("Linking sources together...");
            // std::cout << link_command.str() << std::endl;
            if (int err = system(link_command.str().c_str())) {
                dll.log_error("Error linking to", dll.output_file, ':', err);
                return false;
            }
        }

        dll.log_info("");
        return true;
    }

    void start( dll_callback_func_t* callback_func ) {
        // Open the created shared library.
        dll.handle = (link_map *)dlopen(dll.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (dll.handle == nullptr)
            dll.log_error("Error loading application:", dlerror());

        CHK_PH(plthook_open_by_handle(&dll.plthook, dll.handle));

        set_callback_func_t* set_callback = (set_callback_func_t*)dlsym(dll.handle, "setDLLCallback");
        if (set_callback == nullptr)
            dll.log_error("No setDLLCallback() found, so we can't check for file changes!");
        else
            (*set_callback)(callback_func);

        // Run the main function till we're done.
        typedef int mainFunc(int, char**);
        mainFunc* main_func = (mainFunc*)dlsym(dll.handle, "main");
        if (main_func == nullptr)
            dll.log_error("No main function found, so we can't start the application!");
        else
            (*main_func)(0, nullptr);

        dll.log_info("Ending live reload session");
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
        if (file.has_source_changed()) {
            // TODO: only recompile the actual function that has been changed.
            file.compile(true);
            file.replace_functions();
        }
    }
};

static live_cc_t live_cc;

void callback() {
    live_cc.update();
}

int main(int argn, char** argv) {
    live_cc.parse_arguments(argn, argv);
    if (live_cc.compile_and_link() && live_cc.dll.build_type == LIVE)
        live_cc.start(callback);

    return 0;
}

// TODO: our own command line arguments:
//  -h/--help
// TODO: move static stuff where possible

// Glob support.

// TODO: add dependency map support,
// which compiles all the affected source files like
// normal (so always -c in the build, overwriting the normal .o files)
// and links them together to form the temporary dll.
//