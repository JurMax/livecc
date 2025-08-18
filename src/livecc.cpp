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
    Context context;

    size_t path_index = 0;
    std::vector<SourceFile> files;

    void parse_arguments(int argn, char** argv) {
        context.working_directory = fs::current_path();
        context.output_file = "build/a.out";
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
                        context.output_file = arg.substr(2);
                }
                else if (arg[1] == 'j') {
                    // Set the number of parallel threads to use.
                    if (arg.length() == 2)
                        context.job_count = std::stoi(argv[++i]);
                    else
                        context.job_count = std::stoi(argv[i] + 2);
                }
                else if (arg[1] == 'l' || arg[1] == 'L' || arg.starts_with("-fuse-ld=")) {
                    context.link_arguments += " " + std::string(arg);
                }
                else if (arg[1] == 'I') {
                    std::string_view dir = std::string_view(arg).substr(2);
                    if (dir[0] == '"' && dir[dir.length() - 1] == '"')
                        dir = dir.substr(1, dir.length() - 2);
                    context.build_include_dirs.push_back(dir);
                }
                else if (arg.starts_with("--pch")) {
                    if (arg.length() == 5)
                        next_arg_type = PCH;
                    else
                        files.emplace_back(arg, SourceFile::PCH);
                }
                else if (arg == "--standalone")
                    context.build_type = STANDALONE;
                else if (arg == "--shared")
                    context.build_type = SHARED;
                else if (arg == "--no-rebuild-with-O0")
                    context.rebuild_with_O0 = false;
                else if (arg == "--verbose")
                    context.verbose = true;
                else if (arg == "--test")
                    context.test = true;
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
                    files.emplace_back(arg);
                else if (next_arg_type == PCH)
                    files.emplace_back(arg, SourceFile::PCH);
                else if (next_arg_type == OUTPUT)
                    context.output_file = arg;
                else
                    build_command << ' ' << arg;
                next_arg_type = INPUT;
            }
        }

        // Set the output directory.
        context.output_directory = context.output_file.parent_path();
        switch (context.build_type) {
            case LIVE:       context.output_directory /= "live"; break;
            case SHARED:     context.output_directory /= "shared"; break;
            case STANDALONE: context.output_directory /= "standalone"; break;
        }

        // Rename the output file if no extension has been given.
        if (!context.output_file.has_extension()) {
            std::string new_filename = context.output_file.filename().string();
            switch (context.build_type) {
                case LIVE:   new_filename = "lib" + new_filename + "_live.a"; break;
                case SHARED: new_filename = "lib" + new_filename + ".a"; break;
                case STANDALONE: break;
            }
            context.output_file = context.output_file.parent_path() / new_filename;
        }

        // Create the temporary and the modules directories.
        context.modules_directory = context.output_directory / "modules";
        build_command << " -fprebuilt-module-path=" << context.modules_directory;
        fs::create_directories(context.modules_directory);
        fs::create_directories(context.output_directory / "tmp");
        fs::create_directories(context.output_directory / "system");

        if (context.build_type == LIVE || context.build_type == SHARED) {
            build_command << " -fPIC";
            context.link_arguments += " -shared";
        }
        // if (context.build_type == LIVE)
            // build_command << " -fno-inline";// -fno-ipa-sra";
        // -fno-ipa-sra disables removal of unused parameters, as this breaks code recompiling for functions with unused arguments for some reason.
        build_command << " -MD -Winvalid-pch";

        if (context.test) {
            if (context.build_type == STANDALONE)
                context.log_error("Tests can't be run in standalone mode!");
            else
                build_command << " -DLCC_TEST";
        }

        context.build_command = build_command.str();
    }

    bool compile_files(std::string name, const std::vector<SourceFile*>& to_compile) {
        if (to_compile.size() > 0) {
            context.log_set_task("COMPILING " + name, to_compile.size());
            ThreadPool pool(context.job_count);
            for (SourceFile* f : to_compile)
                pool.enqueue([this, f] {
                    bool error = f->compile(context);
                    context.log_step_task();
                    return error;
                });
            pool.join();
            context.log_clear_task();
            return !pool.got_error;
        }
        return true;
    }

    void add_module_dependencies(std::set<SourceFile*>& to_compile, std::set<SourceFile*>& modules_to_compile) {
        // Find all the files we need to compile because of the changed modules.
        std::stack<SourceFile*> check_stack;
        for (SourceFile* f : modules_to_compile) {
            check_stack.emplace(f);
        }

        // TODO: find a way to check if the exports of a module have changed,
        // instead of recompiling every time anything in it has changed. This
        // SHOULD be possible, this would make modules so much better. If not,
        // then using modules might not be worth it........

        while (!check_stack.empty()) {
            SourceFile* f = check_stack.top();
            check_stack.pop();

            // Iterate over all the file that depend on this file,
            // and add them to the to_compile files.
            for (SourceFile* d : f->dependent_files) {
                auto it = to_compile.find(d);
                if (it == to_compile.end()) {
                    to_compile.emplace_hint(it, d);
                    check_stack.push(d);
                }
            }
        }
    }

    bool compile_files(std::set<SourceFile*>& to_compile) {
        context.log_set_task("COMPILING", to_compile.size());
        ThreadPool pool(context.job_count);

        // Add all files that have no dependencies to the compile queue.
        // As these compile they will add all the other files too.
        for (SourceFile& f : files) {
            if (f.dependencies_count == 0) {
                add_to_compile_queue(pool, to_compile, &f);
            }
        }

        pool.join();
        context.log_clear_task();
        // TODO: find a way to check for files where d->compiled_dependencies != d->dependencies_count,
        // which means that they haven't been compiled.
        return true;
    }

private:
    void mark_compiled(ThreadPool& pool, const std::set<SourceFile*>& to_compile, SourceFile* f) {
        for (SourceFile* d : f->dependent_files) {
            if (++d->compiled_dependencies == d->dependencies_count) {
                add_to_compile_queue(pool, to_compile, d);
            }
        }
    }

    void add_to_compile_queue(ThreadPool& pool,
                              const std::set<SourceFile*>& to_compile, SourceFile* f) {
        if (to_compile.contains(f)) {
            pool.enqueue([this, &pool, &to_compile, f] {
                bool error = f->compile(context);
                if (!error)
                    mark_compiled(pool, to_compile, f);
                context.log_step_task();
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
        std::unordered_map<std::string, SourceFile*> module_map;
        std::unordered_map<fs::path, SourceFile*> header_map;
        for (SourceFile& f : files) {
            if (f.type == SourceFile::MODULE) {
                auto it = module_map.find(f.module_name);
                if (it == module_map.end())
                    module_map.emplace_hint(it, f.module_name, &f);
                else {
                    context.log_error("There are multiple implementations for module ", f.module_name,
                        "(", it->second->source_path, "and", f.source_path, ")");
                    return false;
                }
            }
            else if (f.is_header()) {
                header_map.emplace(f.source_path, &f);
            }
        }

        // Fill the dependent_files of each module, e.g.
        // the files that depend on that module.
        for (SourceFile& f : files) {
            for (const fs::path& header_path : f.header_dependencies) {
                auto header_it = header_map.find(header_path);
                SourceFile* header = header_it->second;
                if (header_it == header_map.end() || &f == header)
                    continue;

                header->dependent_files.push_back(&f);
                ++f.dependencies_count;

                if (f.type == SourceFile::SYSTEM_PCH)
                    f.build_pch_includes += " -include \"" + header->compiled_path.replace_extension().string() + '"';
                f.build_pch_includes += " -include-pch \"" + header->compiled_path.string() + '"';

            }

            for (const std::string& module_name : f.module_dependencies) {
                auto module_it = module_map.find(module_name);
                SourceFile* module = module_it->second;
                if (module_it != module_map.end() || &f == module)
                    continue;
                module->dependent_files.push_back(&f);
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
            std::set<SourceFile*> modules_to_compile;
            std::set<SourceFile*> files_to_compile;

            {
                std::vector<size_t> modules_to_compile_i;
                std::vector<size_t> files_to_compile_i;

                InitData init_data;
                context.log_set_task("LOADING DEPENDENCIES", files.size());
                ThreadPool pool(context.job_count);
                for (size_t i = 0; i < files.size(); ++i)
                    pool.enqueue([&, i] () {
                        SourceFile& file = files[i];
                        if (file.load_dependencies(context, init_data)) {
                            std::unique_lock<std::mutex> lock(init_data.mutex);
                            files_to_compile_i.push_back(i);
                            if (file.type == SourceFile::MODULE)
                                modules_to_compile_i.push_back(i);
                        }
                        context.log_step_task();
                        return false;
                    });
                pool.join();
                context.log_clear_task();

                // Create system header compilation units and mark them for recompilation if necessary.
                for (const fs::path& header_path : init_data.system_headers) {
                    SourceFile& f = files.emplace_back(header_path, SourceFile::SYSTEM_PCH);
                    f.compiled_path = context.output_directory / "system" / (header_path.filename().string() + ".gch");
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

        bool link = did_compilation || !fs::exists(context.output_file);

        // link all the files into one shared library.
        if (link) {
            std::ostringstream link_command;
            link_command << context.build_command;
            link_command << context.link_arguments;
            link_command << " -Wl,-z,defs"; // Make sure that all symbols are resolved.
            link_command << " -o " << context.output_file;
            for (SourceFile& file : files)
                if (!file.is_header())
                    link_command << ' ' << file.compiled_path;

            context.log_info("Linking sources together...");
            // std::cout << link_command.str() << std::endl;

            if (context.verbose)
                context.log_info(link_command.str());

            if (int err = system(link_command.str().c_str())) {
                context.log_error("Error linking to", context.output_file, ':', err);
                return false;
            }
        }

        context.log_info("");
        return true;
    }

    void start( dll_callback_func_t* callback_func ) {
        // Open the created shared library.
        context.handle = (link_map *)dlopen(context.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (context.handle == nullptr)
            context.log_error("Error loading application:", dlerror());

        CHK_PH(plthook_open_by_handle(&context.plthook, context.handle));

        set_callback_func_t* set_callback = (set_callback_func_t*)dlsym(context.handle, "setDLLCallback");
        if (set_callback == nullptr)
            context.log_error("No setDLLCallback() found, so we can't check for file changes!");
        else
            (*set_callback)(callback_func);

        // Run the main function till we're done.
        typedef int mainFunc(int, char**);
        mainFunc* main_func = (mainFunc*)dlsym(context.handle, "main");
        if (main_func == nullptr)
            context.log_error("No main function found, so we can't start the application!");
        else
            (*main_func)(0, nullptr);

        context.log_info("Ending live reload session");
        close();

        plthook_close(context.plthook);
        dlclose(context.handle);
    }

    void close() {
        // Close all the dlls.
        while (!context.loaded_handles.empty()) {
            dlclose(context.loaded_handles.back());
            context.loaded_handles.pop_back();
        }

        // Delete the temporary files.
        while (!context.temporary_files.empty()) {
            fs::remove(context.temporary_files.back());
            context.temporary_files.pop_back();
        }

        files.clear();
    }

    void update() {
        path_index = (path_index + 1) % files.size();

        SourceFile& file = files[path_index];
        if (file.has_source_changed()) {
            // TODO: only recompile the actual function that has been changed.
            file.compile(context, true);
            file.replace_functions(context);
        }
    }

    void run_tests() {
        // Open the created shared library.
        link_map* handle = (link_map *)dlopen(context.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (handle == nullptr)
            context.log_error("Error loading test application:", dlerror());
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

        context.log_info("Running", test_functions.size(), "tests");
        context.log_set_task("TESTING", test_functions.size());
        ThreadPool pool(context.job_count);
        for (auto [name, func]: test_functions)
            pool.enqueue([this, func] {
                func();
                context.log_step_task();
                return false; // TODO: check for errors.
            });
        pool.join();
        context.log_clear_task();
        context.log_info("\n");
        dlclose(handle);
    }
};

static live_cc_t live_cc;

int main(int argn, char** argv) {

    // SourceFile s("/home/jurriaan/programming/overture/include/GEO/util/containers/list.hpp");
    // s.read_dependencies(live_cc.context);
    // return 0;

    live_cc.parse_arguments(argn, argv);

    if (live_cc.compile_and_link()) {
        if (live_cc.context.test)
            live_cc.run_tests();
        else if (live_cc.context.build_type == LIVE)
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
// and links them together to form the temporary context.
//