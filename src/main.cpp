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
#include "context.hpp"
#include "source_file.hpp"
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <stdio.h>
#include <fstream>

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


struct Main {
    Context context;

    // We use a deque so that source files dont need to be moved.
    std::deque<SourceFile> files;

    size_t path_index = 0;

    // Returns true if all the arguments are valid.
    bool parse_arguments(int argn, char** argv) {
        context.working_directory = fs::current_path();
        context.output_file = "build/a.out";
        std::ostringstream build_command;
        std::ostringstream link_arguments;
        build_command << "clang++ "; // TODO add C support.

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
                    link_arguments << arg << ' ';
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
                        files.emplace_back(context, arg, SourceFile::PCH);
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
                    build_command << arg << ' ';

                    // The next argument is part of this flag, so it's not a file.
                    // TODO: handle all the arguments in which you specify an option.
                    // like: -MD, --param, etc.
                    if (arg.length() == 2 || (arg.starts_with("-include") && arg.size() == 8))
                        next_arg_type = FLAG;
                }
            }
            else {
                if (next_arg_type == INPUT) {
                    if (auto type = SourceFile::get_type(arg))
                        files.emplace_back(context, arg, *type);
                    else if (!add_source_directory(arg))
                        context.log_error_title("unknown input supplied: ", arg);
                }
                else if (next_arg_type == PCH)
                    files.emplace_back(context, arg, SourceFile::PCH);
                else if (next_arg_type == OUTPUT)
                    context.output_file = arg;
                else
                    build_command << arg << ' ';
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
        // build_command << " -fprebuilt-module-path=" << context.modules_directory;

        if (context.build_type == LIVE || context.build_type == SHARED) {
            build_command << "-fPIC ";
            link_arguments << "-shared ";
        }
        // if (context.build_type == LIVE)
            // build_command << "-fno-inline ";// -fno-ipa-sra";
        // -fno-ipa-sra disables removal of unused parameters, as this breaks code recompiling for functions with unused arguments for some reason.
        build_command << "-Winvalid-pch ";

        if (context.test) {
            if (context.build_type == STANDALONE)
                context.log_error("Tests can't be run in standalone mode!");
            else
                build_command << "-DLCC_TEST ";
        }

        context.build_command = build_command.str();
        context.link_arguments = link_arguments.str();
        return true;
    }

    /**
     * Add all the source files in a given directory to the build system.
     * Returns true if the given path was actually a directory.
     */
    bool add_source_directory( const std::string_view& dir_path ) {
        std::error_code err;
        for (const fs::directory_entry& dir_entry : fs::recursive_directory_iterator(dir_path, err))
            if (auto type = SourceFile::get_type(dir_entry.path().native()))
                files.emplace_back(context, dir_entry.path(), *type);
        return !err;
    }

    /**
     * Invoke clang and read the previous configuration.
     */
    bool initialise() {
        if (!get_system_include_dirs()) {
            context.log_error("Couldn't find clang, is it in the path?");
            return false;
        }

        // try {
        //     fs::create_directories(context.modules_directory);
        //     fs::create_directories(context.output_directory / "tmp");
        //     fs::create_directories(context.output_directory / "system");
        // }
        // catch (std::exception e) {
        //     context.log_error("Failed to create directories: ", e.what());
        //     return false;
        // }

        context.build_command_changed = have_build_args_changed();
        update_compile_commands(context.build_command_changed);
        return true;
    }


    bool get_system_include_dirs() {
        // TODO: maybe cache this, only check if clang file write time changes on startup.
        char buf[4096];
        FILE* pipe = popen("echo | clang -xc++ -E -v - 2>&1 >/dev/null", "r");
        if (pipe == NULL)
            return false;
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            if (buf[0] == ' ' && buf[1] == '/') {
                size_t len = 2;
                while (buf[len] != '\n' && buf[len] != '\0') ++len;
                buf[len] = '\0';
                context.system_include_dirs.push_back(buf + 1);
            }
        }
        return pclose(pipe) == 0;
    }

    bool have_build_args_changed() {
        // Read build args from file.
        fs::path command_file = (context.output_directory / "command.txt");
        if (FILE* f = fopen(command_file.c_str(), "rb")) {
            char buff[256];
            bool changed = false;
            size_t count, i = 0;
            while ((count = fread(buff, 1, sizeof(buff), f)))
                for (size_t j = 0; j != count; ++j, ++i)
                    if (i == context.build_command.size() || buff[j] != context.build_command[i]) {
                        changed = true;
                        goto stop;
                    }
            if (i != context.build_command.size())
                changed = true;
        stop:
            fclose(f);
            if (!changed)
                return false;
        }

        // Write the new build file.
        if (FILE* f = fopen(command_file.c_str(), "wb")) {
            fwrite(context.build_command.c_str(), 1, context.build_command.length(), f);
            fclose(f);
        }

        return true;
    }

    void update_compile_commands(bool need_update = true) {
        // If a file was not compiled before, we need to recreate the compile_commands.json
        for (auto it = files.begin(), end = files.end(); it != end && !need_update; ++it)
            if (!it->is_header() && !it->compiled_time)
                need_update = true;

        if (need_update) {
            std::ofstream compile_commands("compile_commands.json");
            std::string dir_string = "\t\t\"directory\": \"" + context.working_directory.native() + "\",\n";
            compile_commands << "[\n";
            fs::path output_path;

            bool first = true;
            for (SourceFile& file : files) {
                file.set_compile_path(context);
                if (file.is_header())
                    continue;
                if (!first) compile_commands << ",\n";
                else first = false;
                compile_commands << "\t{\n";
                compile_commands << dir_string;
                compile_commands << "\t\t\"command\": \"";
                std::string command = file.get_build_command(context, false, &output_path);
                for (char c : command) {
                    if (c == '"')
                        compile_commands.put('\\');
                    compile_commands.put(c);
                }
                compile_commands << "\",\n";
                compile_commands << "\t\t\"file\": \""
                    << file.source_path.native() << "\"\n";
                compile_commands << "\t}";
            }
            compile_commands << "\n]\n";
        }
    }

public:
    struct DependencyTreeBuilder {
        Context& context;
        std::deque<SourceFile>& files;

        ThreadPool pool;
        std::unordered_map<fs::path, SourceFile*> header_map;
        std::unordered_map<std::string, SourceFile*> module_map;

        std::mutex header_mutex;
        std::mutex module_mutex;

        // Returns true on success.
        bool build() {
            context.log_set_task("LOADING DEPENDENCIES", files.size());

            // Initialise the maps.
            for (SourceFile& f : files) {
                if (f.type == SourceFile::MODULE) {
                    auto it = module_map.find(f.module_name);
                    if (it == module_map.end())
                        module_map.emplace(f.module_name, &f);
                    else {
                        context.log_error("There are multiple implementations for module ", f.module_name,
                            "(in ", it->second->source_path, " and ", f.source_path, ")");
                        return false;
                    }
                }
                else if (f.is_header())
                    header_map.emplace(f.source_path, &f);
            }

            // Read all the files. Store the size to avoid mapping a
            // header that was added later twice.
            for (size_t i = 0, l = files.size(); i != l; ++i)
                pool.enqueue([&, &file = files[i]] {
                    return map_file_dependencies(file);
                });
            pool.join();
            context.log_clear_task();
            return true;
        }

    private:
        bool map_file_dependencies(SourceFile& file) {
            file.read_dependencies(context);

            if (!file.header_dependencies.empty()) {
                // TODO: create build_pch_includes maybe?.

                std::lock_guard<std::mutex> lock(header_mutex);
                for (const auto& [header_path, type] : file.header_dependencies) {
                    auto it = header_map.find(header_path);
                    if (it == header_map.end()) {
                        // Insert the header as a new source file.
                        SourceFile& header = files.emplace_back(context, header_path, type);
                        it = header_map.emplace(header_path, &header).first;
                        pool.enqueue([&] -> bool {
                            return map_file_dependencies(header);
                        });
                        context.bar_task_total++;
                    }

                    it->second->dependent_files.push_back(&file);
                    file.dependencies_count++;
                }
            }

            if (!file.module_dependencies.empty()) {
                std::lock_guard<std::mutex> lock(module_mutex);
                for (const std::string& module : file.module_dependencies) {
                    auto it = module_map.find(module);
                    if (it != module_map.end()) {
                        it->second->dependent_files.push_back(&file);
                        file.dependencies_count++;
                    }
                    else {
                        context.log_error("Module ", module, " imported in ", file.source_path, " does not exist");
                    }
                }
            }

            context.log_step_task();
            return false;
        }

    public:
        // Returns true if at least 1 file should be compiled.
        uint mark_for_compilation() {
            uint compile_count = 0;
            if (context.build_command_changed) {
                for (SourceFile& file : files)
                    file.need_compile = true;
                compile_count += files.size();
            }
            else {
                for (SourceFile& file : files)
                    if (file.dependencies_count == 0)
                        compile_count += check_file_for_compilation(file);
            }
            return compile_count;
        }

    private:
        uint check_file_for_compilation(SourceFile& file) {
            if (file.has_changed)
                return mark_file_for_compilation(file);
            else {
                file.visited = true;
                uint compile_count = 0;
                for (SourceFile* child : file.dependent_files)
                    if (!child->visited)
                        compile_count += check_file_for_compilation(*child);
                return compile_count;
            }
        }

        uint mark_file_for_compilation(SourceFile& file) {
            uint compile_count = 1;
            file.need_compile = true;
            file.visited = true;
            for (SourceFile* child : file.dependent_files)
                if (!child->need_compile)
                    compile_count += mark_file_for_compilation(*child);
            return compile_count;
        }
    };

    struct Compiler {
        Context& context;
        std::deque<SourceFile>& files;
        ThreadPool pool;

        bool compile_files(size_t compile_count) {
            context.log_set_task("COMPILING", compile_count);

            // Add all files that have no dependencies to the compile queue.
            // As these compile they will add all the other files too.
            for (SourceFile& file : files)
                if (file.dependencies_count == 0)
                    add_to_compile_queue(file);

            pool.join();
            context.log_clear_task();

            bool everything_compiled = true;
            bool compilations_failed = false;
            for (auto it = files.begin(), end = files.end(); it != end && everything_compiled; ++it) {
                if (it->compilation_failed)
                    compilations_failed = true;
                if (compilations_failed || it->compiled_dependencies != it->dependencies_count)
                    everything_compiled = false;
            }

            if (!everything_compiled) {
                if (compilations_failed) {
                    context.log_info();
                    context.log_error_title("compilation failed for:");
                    for (SourceFile& file : files)
                        if (file.compilation_failed)
                            context.log_error("        ", file.source_path);
                }
                else {
                    context.log_info();
                    context.log_error_title("circular dependencies found:");
                    for (SourceFile& file : files) {
                        if (file.compiled_dependencies != file.dependencies_count) {
                            if (depends_on(file, file)) {
                                std::cout << "        ";
                                depends_on_print(file, file);
                                std::cout << " -> " << file.source_path << std::endl;
                            }
                        }
                    }
                }
            }

            return everything_compiled;
        }

        /** Return true if the given depends (indirectly) on the given dependency. Is slow */
        bool depends_on(SourceFile& file, SourceFile& dependency) {
            for (SourceFile* dependent : dependency.dependent_files) {
                if (dependent == &file) return true;
                else if (depends_on(file, *dependent)) return true;
            }
            return false;
        }
        bool depends_on_print(SourceFile& file, SourceFile& dependency) {
            for (SourceFile* dependent : dependency.dependent_files) {
                if (dependent == &file) {
                    std::cout << dependent->source_path;
                    return true;
                }
                else if (depends_on_print(file, *dependent)) {
                    std::cout << " -> " << dependent->source_path;
                    return true;
                }
            }
            return false;
        }

    private:
        void add_to_compile_queue(SourceFile& file) {
            if (file.need_compile) {
                pool.enqueue([&] -> bool {
                    bool success = file.compile(context);
                    if (success)
                        mark_compiled(file);
                    context.log_step_task();
                    return !success;
                });
            }
            else // We don't need to compile this file, so add its dependencies to the queue.
                mark_compiled(file);
        }

        void mark_compiled(SourceFile& file) {
            for (SourceFile* child : file.dependent_files)
                if (++child->compiled_dependencies == child->dependencies_count)
                    add_to_compile_queue(*child);
        }
    };

    bool compile_and_link() {

        // Make sure all the files are compiled.
        uint compile_count;
        {
            // Read to files to get all the dependencies.
            DependencyTreeBuilder builder{context, files, context.job_count};
            if (!builder.build())
                return false;
            compile_count = builder.mark_for_compilation();
        }
        if (compile_count != 0) {
            Compiler compiler{context, files, context.job_count};
            if (!compiler.compile_files(compile_count))
                return false;
        }

        bool do_link = compile_count != 0u || !fs::exists(context.output_file);

        // link all the files into one shared library.
        if (do_link) {
            std::ostringstream link_command;
            link_command << context.build_command;
            link_command << context.link_arguments;
            link_command << "-Wl,-z,defs "; // Make sure that all symbols are resolved.
            link_command << "-o " << context.output_file;
            for (SourceFile& file : files)
                if (!file.is_header())
                    link_command << ' ' << file.compiled_path;
            link_command << '\0';

            context.log_info("Linking sources together...");
            // std::cout << link_command.str() << std::endl;

            if (context.verbose)
                context.log_info(link_command.view());

            if (int err = system(link_command.view().data())) {
                context.log_error("Error linking to ", context.output_file, ": ", err);
                return false;
            }
        }

        context.log_info("");
        return true;
    }

public:
    void start(dll_callback_func_t* callback_func) {
        // Open the created shared library.
        context.handle = (link_map *)dlopen(context.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (context.handle == nullptr)
            context.log_error("Loading application failed:", dlerror());

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
            if (file.compile(context, true)) {
                file.replace_functions(context);
                context.log_info("Done!");
            }
        }
    }

    void run_tests() {
        // Open the created shared library.
        link_map* handle = (link_map *)dlopen(context.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (handle == nullptr)
            context.log_error("Error loading test application: ", dlerror());
        typedef void (*Func)(void);
        std::vector<std::pair<const char*, Func>> test_functions;
        std::string_view str_table = get_string_table(handle);
        if (str_table.size() > 5)
            for (size_t i = 0, l = str_table.size() - 5; i < l; ++i)
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

        context.log_info("Running ", test_functions.size(), " tests");
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

static Main* main_ptr;

int main(int argn, char** argv) {
    Main main;
    main_ptr = &main;
    if (!main.parse_arguments(argn, argv)) {
        main.context.log_error("Failed parsing some arguments");
        // TODO: show help.
        return 1;
    }

    if (main.files.empty()) {
        main.add_source_directory("src");
        if (main.files.empty()) {
            // TODO: show help.
            main.context.log_error("No input files");
            return 2;
        }
    }

    if (!main.initialise()) {
        return 1;
    }

    if (main.compile_and_link()) {
        if (main.context.test)
            main.run_tests();
        else if (main.context.build_type == LIVE)
            main.start([] () { main_ptr->update(); });
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