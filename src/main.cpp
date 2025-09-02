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

#include "dependency_tree.hpp"
#include "compile.hpp"
#include "thread_pool.hpp"

#include <charconv>
#include <elf.h>
#include <filesystem>
#include <link.h>
#include <string>
#include <system_error>
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
using namespace std::literals;

typedef void dll_callback_func_t(void);
typedef int set_callback_func_t(dll_callback_func_t*);

// Get the table of string from a link map handle.
static std::string_view get_string_table(link_map* handle) {
    size_t str_table_size = 0;
    const char* str_table = nullptr;
    for (auto ptr = handle->l_ld; ptr->d_tag; ++ptr) {
        if (ptr->d_tag == DT_STRTAB) str_table = (const char*)ptr->d_un.d_ptr;
        else if (ptr->d_tag == DT_STRSZ) str_table_size = ptr->d_un.d_val;
    }
    return {str_table, str_table_size};
}

struct Main {
    Context context;

    // We use a deque so that source files dont need to be moved.
    std::deque<SourceFile> files;
    DependencyTree dependency_tree;

    size_t path_index = 0;

    // Returns true if all the arguments are valid.
    bool parse_arguments(int argn, char** argv) {
        std::error_code err;
        context.working_directory = fs::current_path();
        context.output_file = "build/a.out";
        std::ostringstream build_command;
        std::ostringstream link_arguments;

        if (const char* env_compiler = std::getenv("CXX"))
            context.compiler = env_compiler;
        else if (const char* env_compiler = std::getenv("CC"))
            context.compiler = env_compiler;
        if (context.compiler.contains("gcc") || context.compiler.contains("g++"))
            context.compiler_type = Context::GCC;

        build_command << context.compiler << ' ';

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
                    uint job_count;
                    std::string_view value = arg.length() == 2 ? argv[++i] : arg.substr(2);
                    if (std::from_chars(value.begin(), value.end(), job_count).ec == std::errc{})
                        context.job_count = job_count;
                    else context.log_error("invalid job count value: ", value);
                }
                else if (arg[1] == 'l' || arg[1] == 'L' || arg.starts_with("-fuse-ld=")) {
                    link_arguments << arg << ' ';
                }
                else if (arg[1] == 'I') {
                    std::string_view dir = arg.substr(2);
                    if (dir[0] == '"' && dir[dir.length() - 1] == '"')
                        dir = dir.substr(1, dir.length() - 2);
                    context.build_include_dirs.push_back(dir);
                    build_command << "-I\"" << dir << "\" ";
                }
                else if (arg.starts_with("--pch")) {
                    if (arg.length() == 5) next_arg_type = PCH;
                    else files.emplace_back(context, arg, SourceFile::PCH);
                }
                else if (arg == "--standalone") context.build_type = STANDALONE;
                else if (arg == "--shared") context.build_type = SHARED;
                else if (arg == "--no-rebuild-with-O0") context.rebuild_with_O0 = false;
                else if (arg == "--verbose") context.verbose = true;
                else if (arg == "--test") context.test = true;
                else if (arg == "--no-header-units") context.use_header_units = false;
                else if (arg == "--header-units") context.use_header_units = true;
                else if (arg.starts_with("-std=c++")) context.cpp_version = arg;
                else if (arg.starts_with("-std=c")) context.c_version = arg;
                else if (arg.starts_with("-fuse-ld=")) {
                    link_arguments << arg << ' ';
                    context.custom_linker_set = true;
                }
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
                        context.log_error("unknown input supplied: ", arg);
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
                context.log_error("tests can't be run in standalone mode!");
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
     * Invoke the compiler and read the previous configuration.
     */
    bool initialise() {
        if (!get_system_include_dirs()) {
            context.log_error("couldn't find ", context.compiler, ". is it in the path?");
            return false;
        }
        context.build_command_changed = have_build_args_changed();
        update_compile_commands(context.build_command_changed);
        return true;
    }


    bool get_system_include_dirs() {
        // TODO: maybe cache this, only check if the compiler exe file write time changes on startup.
        char buf[4096];
        std::string command = std::format("echo | {} -xc++ -E -v - 2>&1 >/dev/null", context.compiler);
        FILE* pipe = popen(command.c_str(), "r");
        FILE* mold_pipe = context.custom_linker_set ? nullptr : popen("mold -v &>/dev/null", "r");
        if (pipe == nullptr)
            return false;
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            if (buf[0] == ' ' && buf[1] == '/') {
                size_t len = 2;
                while (buf[len] != '\n' && buf[len] != '\0') ++len;
                buf[len] = '\0';
                context.system_include_dirs.push_back(buf + 1);
            }
        }

        // Check if mold exists, and use it as the default linker if it does.
        if (mold_pipe != nullptr && pclose(mold_pipe) == 0)
            context.link_arguments += "-fuse-ld=mold ";
        return pclose(pipe) == 0;
    }

    bool have_build_args_changed() {
        // TODO: check if the compiler or livecc version updated by checking
        // if their file changes is higher than command.txt

        std::vector<char> build_command;
        build_command.reserve(context.build_command.size()
            + context.cpp_version.size() + context.c_version.size() + 2);
        build_command.append_range(context.build_command);
        build_command.append_range(context.cpp_version);
        build_command.push_back(' ');
        build_command.append_range(context.c_version);
        build_command.push_back(' ');

        // Read build args from file.
        fs::path command_file = context.output_directory / "command.txt";
        if (FILE* f = fopen(command_file.c_str(), "rb")) {
            char buff[256];
            bool changed = false;
            size_t count, i = 0;
            while ((count = fread(buff, 1, sizeof(buff), f)))
                for (size_t j = 0; j != count; ++j, ++i)
                    if (i == build_command.size() || buff[j] != build_command[i]) {
                        changed = true;
                        goto stop;
                    }
            if (i != build_command.size())
                changed = true;
        stop:
            fclose(f);
            if (!changed)
                return false;
        }

        // Write the new build file.
        if (FILE* f = fopen(command_file.c_str(), "wb")) {
            fwrite(build_command.data(), 1, build_command.size(), f);
            fclose(f);
        }

        return true;
    }

    void update_compile_commands(bool need_update = true) {
        // TODO: maybe do this after compilation. Then you dont need to update
        // compile_path separately, and you also have the modules present.

        // If a file was not compiled before, we need to recreate the compile_commands.json
        for (auto it = files.begin(), end = files.end(); it != end && !need_update; ++it)
            if (!it->is_include() && !it->compiled_time)
                need_update = true;

        if (need_update) {
            std::ofstream compile_commands("compile_commands.json");
            std::string dir_string = "\t\t\"directory\": \"" + context.working_directory.native() + "\",\n";
            compile_commands << "[\n";

            bool first = true;
            for (SourceFile& file : files) {
                file.set_compile_path(context);
                if (file.is_include())
                    continue;
                if (!first) compile_commands << ",\n";
                else first = false;
                compile_commands << "\t{\n";
                compile_commands << dir_string;
                compile_commands << "\t\t\"command\": \"";
                // TODO: dont use the full build command, its way too big
                std::string command = file.get_build_command(context);
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

    bool compile_and_link() {
        // Read to files to get all the dependencies.
        if (!dependency_tree.build(context, files))
            return false;

        uint compile_count = dependency_tree.mark_for_compilation(context, files);
        if (compile_count != 0 && !compile_all(context, files))
            return false;

        bool do_link = compile_count != 0u || !fs::exists(context.output_file);

        // link all the files into one shared library.
        if (do_link) {
            std::ostringstream link_command;
            link_command << context.build_command;
            link_command << context.link_arguments;
            link_command << "-Wl,-z,defs "; // Make sure that all symbols are resolved.
            link_command << "-o " << context.output_file;
            for (SourceFile& file : files)
                if (!file.is_include())
                    link_command << ' ' << file.compiled_path;
            link_command << '\0';

            context.log_info("Linking sources together...");
            // std::cout << link_command.str() << std::endl;

            if (context.verbose)
                context.log_info(link_command.view());

            if (int err = system(link_command.view().data())) {
                context.log_error("error linking to ", context.output_file, ": ", err);
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
            context.log_error("loading application failed:", dlerror());

        if (plthook_open_by_handle(&context.plthook, context.handle) != 0) {
            context.log_error("plthook error: ", plthook_error());
            return;
        }

        set_callback_func_t* set_callback = (set_callback_func_t*)dlsym(context.handle, "setDLLCallback");
        if (set_callback == nullptr)
            context.log_info("no setDLLCallback() found, so we can't check for file changes!");
        else
            (*set_callback)(callback_func);

        // Run the main function till we're done.
        typedef int mainFunc(int, char**);
        mainFunc* main_func = (mainFunc*)dlsym(context.handle, "main");
        if (main_func == nullptr)
            context.log_info("no main function found, so we can't start the application!");
        else
            (*main_func)(0, nullptr);

        context.log_info("ending live reload session");
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

    void load_and_replace_functions(const fs::path& obj_path) {
        link_map* handle = (link_map*)dlopen(obj_path.c_str(), RTLD_LAZY | RTLD_GLOBAL | RTLD_DEEPBIND);
        if (handle == nullptr) {
            context.log_info("Error loading ", obj_path);
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

    void update() {
        path_index = (path_index + 1) % files.size();

        SourceFile& file = files[path_index];
        if (file.has_source_changed()) {
            context.log_info(file.source_path, " changed!");
            // TODO: only recompile the actual function that has been changed.

            fs::path output_path = context.output_directory / "tmp"
                    / ("tmp" + (std::to_string(context.temporary_files.size()) + ".so"));
            context.temporary_files.push_back(output_path);
            if (compile_file(context, file, output_path, true)) {
                load_and_replace_functions(output_path);
                context.log_info("Done!");
            }
        }
    }

    void run_tests() {
        // Open the created shared library.
        link_map* handle = (link_map *)dlopen(context.output_file.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (handle == nullptr)
            context.log_error("error loading test application: ", dlerror());
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
        main.context.log_error("failed parsing some arguments");
        // TODO: show help.
        return 1;
    }

    if (main.files.empty()) {
        main.add_source_directory("src");
        if (main.files.empty()) {
            // TODO: show help.
            main.context.log_info("no input files");
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