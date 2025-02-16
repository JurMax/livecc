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

#include "plthook/plthook.h"

#include "thread_pool.hpp"
#include "globals.hpp"
#include "source_file.hpp"
#include <stack>

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
                else if (arg[1] == 'l' || arg[1] == 'L' || arg.starts_with("-fuse-ld=")) {
                    dll.link_arguments += " " + std::string(arg);
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
                else if (arg == "--verbose")
                    dll.verbose = true;
                else if (arg == "--test")
                    dll.test = true;
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
        // if (dll.build_type == LIVE)
            // build_command << " -fno-inline";// -fno-ipa-sra";
        // -fno-ipa-sra disables removal of unused parameters, as this breaks code recompiling for functions with unused arguments for some reason.
        build_command << " -MD -Winvalid-pch";

        if (dll.test) {
            if (dll.build_type == STANDALONE)
                dll.log_error("Tests can't be run in standalone mode!");
            else
                build_command << " -DLCC_TEST";
        }

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
        // TODO: find a way to check for files where d->compiled_dependencies != d->dependencies_count,
        // which means that they haven't been compiled.
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

        // Create a map from module name/header path path to their file object.
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

        // // Remove all deep dependencies. (E.g. and dependency that another file also has as a dependency. )
        // for (source_file_t& f : files) {
        //     for (const fs::path& header : f.header_dependencies) {
        //         auto it = header_map.find(header);
        //         if (it != header_map.end() && &f != it->second) {
        //         }
        //     }
        // }


        // Fill the dependent_files of each module, e.g.
        // the files that depend on that module.
        for (source_file_t& f : files) {
            for (source_file_t& header : f.get_header_dependencies(header_map)) {
                header.dependent_files.push_back(&f);
                ++f.dependencies_count;

                if (f.type == source_file_t::SYSTEM_PCH)
                    f.build_pch_includes += " -include \"" + header.compiled_path.replace_extension().string() + '"';
                f.build_pch_includes += " -include-pch \"" + header.compiled_path.string() + '"';
            }
            for (source_file_t& mod : f.get_module_dependencies(module_map)) {
                mod.dependent_files.push_back(&f);
                ++f.dependencies_count;
            }
        }

        // Remove all the

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
            link_command << " -Wl,-z,defs"; // Make sure that all symbols are resolved.
            link_command << " -o " << dll.output_file;
            for (source_file_t& file : files)
                if (!file.typeIsPCH())
                    link_command << ' ' << file.compiled_path;

            dll.log_info("Linking sources together...");
            // std::cout << link_command.str() << std::endl;

            if (dll.verbose)
                dll.log_info(link_command.str());

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

    void run_tests() {
        // Open the created shared library.
        link_map* handle = (link_map *)dlopen(dll.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (handle == nullptr)
            dll.log_error("Error loading test application:", dlerror());
        typedef void (*Func)(void);
        std::vector<std::pair<const char*, Func>> test_functions;
        std::string_view str_table = get_string_table(handle);
        for (size_t i = 0, l = str_table.size() - 5; i < l; ++i) {
            if (str_table[i] == '\0') {
                const char* name = &str_table[++i];
                if (str_table[i++] == '_' &&
                    str_table[i++] == '_' &&
                    str_table[i++] == 't' &&
                    str_table[i++] == 'e' &&
                    str_table[i++] == 's' &&
                    str_table[i++] == 't' &&
                    str_table[i++] == '_') {
                    Func func = (Func)dlsym(handle, name);
                    if (func != nullptr)
                        test_functions.emplace_back(name, func);
                }
            }
        }

        dll.log_info("Running", test_functions.size(), "tests");
        dll.log_set_task("TESTING", test_functions.size());
        ThreadPool pool(dll.job_count);
        for (auto [name, func]: test_functions)
            pool.enqueue([this, func] {
                func();
                dll.log_step_task();
                return false; // TODO: check for errors.
            });
        pool.join();
        dll.log_clear_task();
        dll.log_info("\n");
        dlclose(handle);
    }
};

static live_cc_t live_cc;

int main(int argn, char** argv) {
    live_cc.parse_arguments(argn, argv);



    if (live_cc.compile_and_link()) {
        if (live_cc.dll.test)
            live_cc.run_tests();
        else if (live_cc.dll.build_type == LIVE)
            live_cc.start([] () { live_cc.update(); });
    }

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