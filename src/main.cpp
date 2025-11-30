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


#include "dependency_tree.hpp"
#include "compile.hpp"
#include "thread_pool.hpp"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <fstream>
#include <cstdio>

#include "platform.hpp"
#include "plthook/plthook.h"


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


/**
 * Add all the source files in a given directory to the build system.
 * Returns true if the given path was actually a directory.
 */
ErrorCode add_source_directory(std::vector<InputFile>& files, std::string_view const& dir_path) {
    std::error_code err;
    for (const fs::directory_entry& dir_entry : fs::recursive_directory_iterator(dir_path, err))
        if (auto type = SourceType::from_extension(dir_entry.path().native()))
            files.emplace_back(dir_entry.path(), *type);
    return !err ? ErrorCode::OK : ErrorCode::OPEN_FAILED;
}

// Returns true if all the arguments are valid.
ErrorCode parse_arguments(Context& context, std::vector<InputFile>& files, std::span<std::string> args) {
    std::error_code err;
    Context::Settings& settings = context.settings;
    std::ostringstream build_command;
    std::ostringstream link_arguments;

    if (const char* env_compiler = std::getenv("CXX"))
        settings.compiler = env_compiler;
    else if (const char* env_compiler = std::getenv("CC"))
        settings.compiler = env_compiler;
    if (settings.compiler.contains("gcc")
        || (settings.compiler.contains("g++")
            && !settings.compiler.contains("clang++")))
        settings.compiler_type = Context::Settings::GCC;

    build_command << settings.compiler << ' ';
    build_command << "-fdiagnostics-color=always -Wpedantic -Wall -Wextra -Winvalid-pch -Wsuggest-override ";
    link_arguments << "-lm -lc++ -lstdc++ -lstdc++exp ";

    enum { INPUT, OUTPUT, PCH, PCH_CPP, FLAG } next_arg_type = INPUT;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg(args[i]);

        if (arg[0] == '-') {
            if (arg[1] == 'o') {
                // Get the output file.
                if (arg.length() == 2)
                    next_arg_type = OUTPUT;
                else
                    settings.output_name = arg.substr(2);
            }
            else if (arg[1] == 'j') {
                // Set the number of parallel threads to use.
                uint job_count;
                std::string_view value = arg.length() == 2 ? args[++i] : arg.substr(2);
                if (std::from_chars(value.begin(), value.end(), job_count).ec == std::errc{})
                    settings.job_count = job_count;
                else context.log.error("invalid job count value: ", value);
            }
            else if (arg[1] == 'l' || arg[1] == 'L' || arg.starts_with("-fuse-ld=") || arg.starts_with("-Wl")) {
                link_arguments << arg << ' ';
            }
            else if (arg[1] == 'I') {
                std::string_view dir = arg.substr(2);
                if (dir[0] == '"' && dir[dir.length() - 1] == '"')
                    dir = dir.substr(1, dir.length() - 2);
                settings.build_include_dirs.push_back(dir);
                build_command << "-I\"" << dir << "\" ";
            }
            else if (arg == "--pch") next_arg_type = PCH;
            else if (arg == "--c++pch") next_arg_type = PCH_CPP;
            else if (arg == "--standalone") settings.build_type = BuildType::STANDALONE;
            else if (arg == "--shared") settings.build_type = BuildType::SHARED;
            else if (arg == "--no-rebuild-with-O0") settings.rebuild_with_O0 = false;
            else if (arg == "--verbose") settings.verbose = true;
            else if (arg == "--test") settings.test = true;
            else if (arg == "--clean") { settings.clean = true; settings.do_compile = false; }
            else if (arg == "--start-clean") settings.clean = true;
            else if (arg == "--header-units") settings.use_header_units = true;
            else if (arg == "--no-header-units") settings.use_header_units = false;
            else if (arg.starts_with("-std=c++")) settings.cpp_version = arg;
            else if (arg.starts_with("-std=c")) settings.c_version = arg;
            else if (arg.starts_with("-fuse-ld=")) {
                link_arguments << arg << ' ';
                settings.custom_linker_set = true;
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
                if (auto type = SourceType::from_extension(arg))
                    files.emplace_back(arg, *type);
                else if (add_source_directory(files, arg) != ErrorCode::OK)
                    context.log.error("unknown input supplied: ", arg);
            }
            else if (next_arg_type == PCH)
                files.emplace_back(arg, arg.ends_with(".h") ? SourceType::C_PCH : SourceType::PCH);
            else if (next_arg_type == PCH_CPP)
                files.emplace_back(arg, SourceType::PCH);
            else if (next_arg_type == OUTPUT)
                settings.output_name = arg;
            else
                build_command << arg << ' ';
            next_arg_type = INPUT;
        }
    }

    // Set the output directory.
    switch (settings.build_type) {
        case BuildType::LIVE:       settings.output_dir = settings.build_dir / "live"; break;
        case BuildType::SHARED:     settings.output_dir = settings.build_dir / "shared"; break;
        case BuildType::STANDALONE: settings.output_dir = settings.build_dir / "standalone"; break;
    }

    // Set the output file path.
    for (char& c : settings.output_name) if (c == '/') c = '_';
    std::string output_filename;
    switch (settings.build_type) {
        case BuildType::LIVE:   output_filename = "lib" + settings.output_name + "_live.a"; break;
        case BuildType::SHARED: output_filename = "lib" + settings.output_name + ".a"; break;
        case BuildType::STANDALONE: output_filename = settings.output_name; break;
    }
    settings.output_file = settings.build_dir / output_filename;

    if (settings.build_type == BuildType::LIVE || settings.build_type == BuildType::SHARED) {
        build_command << "-fPIC ";
        link_arguments << "-shared ";
    }
    // if (context.build_type == LIVE)
        // build_command << "-fno-inline ";// -fno-ipa-sra";
    // -fno-ipa-sra disables removal of unused parameters, as this breaks code recompiling for functions with unused arguments for some reason.

    if (settings.test) {
        if (settings.build_type == BuildType::STANDALONE)
            context.log.error("tests can't be run in standalone mode!");
        else
            build_command << "-DLCC_TEST ";
    }

    // Turn all explicitly passed headers into header units.
    if (settings.use_header_units)
        for (InputFile& file : files)
            if (file.type == SourceType::HEADER)
                file.type = SourceType::HEADER_UNIT;

    settings.build_command = build_command.str();
    settings.link_arguments = link_arguments.str();
    return ErrorCode::OK;
}


bool have_build_args_changed(Context::Settings const& settings) {
    // TODO: check if the compiler  version updated by checking
    // if their file changes is higher than command.txt

    std::vector<char> build_command;
    build_command.reserve(settings.build_command.size()
        + settings.cpp_version.size() + settings.c_version.size() + 2);
    build_command.append_range(settings.build_command);
    build_command.append_range(settings.cpp_version);
    build_command.push_back(' ');
    build_command.append_range(settings.c_version);
    build_command.push_back(' ');

    // Read build args from file.
    fs::path command_file = settings.output_dir / "command.txt";
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

ErrorCode get_system_include_dirs(Context& context) {
    // TODO: maybe cache this, only check if the compiler exe file write time changes on startup.
    char buf[4096];
    std::string command = std::format("echo | {} -xc++ -E -v - 2>&1 >/dev/null", context.settings.compiler);
    FILE* pipe = popen(command.c_str(), "r");
    FILE* mold_pipe = context.settings.custom_linker_set ? nullptr : popen("mold -v &>/dev/null", "r");
    if (pipe == nullptr) {
        context.log.error("couldn't find ", context.settings.compiler, ". is it in the path?");
        return ErrorCode::OPEN_FAILED;
    }

    context.settings.system_include_dirs.clear();
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        if (buf[0] == ' ' && buf[1] == '/') {
            size_t len = 2;
            while (buf[len] != '\n' && buf[len] != '\0') ++len;
            buf[len] = '\0';
            context.settings.system_include_dirs.push_back(buf + 1);
        }
    }

    // Check if mold exists, and use it as the default linker if it does.
    if (mold_pipe != nullptr && pclose(mold_pipe) == 0)
        context.settings.link_arguments += "-fuse-ld=mold ";
    int err = pclose(pipe);
    if (err != 0) {
        context.log.error("compiler returned with error code ", err);
        return ErrorCode::FAILED;
    }
    else return ErrorCode::OK;
}

void update_compile_commands(Context::Settings const& settings, std::span<SourceFile> files) {
    // If a file was not compiled before, we need to recreate the compile_commands.json
    bool create_compile_commands = false;
    for (SourceFile& file : files)
        if (!file.compiled_time && !file.type.is_include()) {
            create_compile_commands = true;
            break;
        }

    // If the build args changed, delete all the compiled files so they have to be recompiled.
    if (have_build_args_changed(settings)) {
        create_compile_commands = true;
        std::error_code err;
        for (SourceFile& file : files) {
            fs::remove(file.compiled_path, err);
            file.compiled_time.reset();
        }
    }

    if (create_compile_commands) {
        // TODO: maybe do this after compilation. Then you dont need to update
        // compile_path separately, and you also have the modules present.
        std::ofstream compile_commands("compile_commands.json");
        if (!compile_commands.is_open()) return;
        std::string dir_string = "\t\t\"directory\": \"" + settings.working_dir.native() + "\",\n";
        compile_commands << "[\n";

        bool first = true;
        for (SourceFile& file : files) {
            if (file.type.is_include())
                continue;
            if (!first) compile_commands << ",\n";
            else first = false;
            compile_commands << "\t{\n";
            compile_commands << dir_string;
            compile_commands << "\t\t\"command\": \"";
            // TODO: dont use the full build command, its way too big
            std::string command = file.get_build_command(settings);
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

ErrorCode compile_and_link(Context const& context, std::span<SourceFile> files) {
    if (compile_all(context, files) != ErrorCode::OK)
        return ErrorCode::FAILED;

    // link all the files into one shared library.
    bool added_shared_library = false;
    std::ostringstream link_command;
    link_command << context.settings.build_command;
    link_command << context.settings.link_arguments;
    if (context.settings.build_type != BuildType::STANDALONE)
        link_command << "-Wl,-z,defs "sv; // Make sure that all symbols are resolved.
    link_command << "-o "sv << context.settings.output_file;
    for (SourceFile& file : files)
        switch (file.type) {
            case SourceType::UNIT:
            case SourceType::C_UNIT:
            case SourceType::MODULE:
                link_command << ' ' << file.compiled_path; break;
            case SourceType::OBJECT:
            case SourceType::STATIC_LIBRARY:
                link_command << ' ' << file.source_path; break;
            case SourceType::SHARED_LIBRARY:
                if (!added_shared_library) {
                    added_shared_library = true;
                    // Look for shared libraries in the same directory.
                    link_command << " -Wl,-rpath,'$ORIGIN'"sv;
                    link_command << " -L" << context.settings.build_dir;
                }
                link_command << " -l:"sv << file.compiled_path.filename();
            default: break;
        }
    link_command << '\0';

    context.log.info("Linking sources together...");
    // context.log.info(link_command.str());

    if (context.settings.verbose)
        context.log.info(link_command.view());

    if (int err = system(link_command.view().data())) {
        context.log.error("error linking to ", context.settings.output_file, ": ", err);
        return ErrorCode::FAILED;
    }

    context.log.info("");
    return ErrorCode::OK;
}

struct Runtime;
static Runtime* runtime;
struct Runtime {
private:
    Context const& context;
    std::span<SourceFile> files;
    uint path_index = 0;

    DLL main_dll;
    plthook_t* plthook = nullptr;
    std::vector<DLL> loaded_dlls;
    std::vector<fs::path> temporary_files;

    static void callback() { runtime->update(); }

public:
    Runtime( Context const& context, std::span<SourceFile> files)
        : context(context), files(files) {

        // Open the created shared library.
        this->main_dll = DLL::open_global(context.log, context.settings.output_file.c_str());

        if (this->main_dll) {
            if (plthook_open_by_handle(&this->plthook, this->main_dll.handle) != 0) {
                context.log.error("plthook error: ", plthook_error());
                return;
            }

            set_callback_func_t* set_callback = (set_callback_func_t*)this->main_dll.symbol("setDLLCallback");
            if (set_callback == nullptr)
                context.log.info("no setDLLCallback() found, so we can't check for file changes!");
            else {
                runtime = this;
                (*set_callback)(callback);
            }
        }
    }

    ~Runtime() {
        if (this->plthook)
            plthook_close(this->plthook);

        // Close all the dlls.
        while (!this->loaded_dlls.empty())
            this->loaded_dlls.pop_back();
        this->main_dll.close();

        // Delete the temporary files.
        while (!this->temporary_files.empty()) {
            fs::remove(this->temporary_files.back());
            this->temporary_files.pop_back();
        }
    }

    void run(char const* func_name = "main") {
        // Run the main function till we're done.
        if (this->main_dll) {
            auto main_func = (int(*)(int, char**))this->main_dll.symbol(func_name);
            if (main_func == nullptr)
                context.log.info(func_name, " not found, so we can't start the application!");
            else
                (*main_func)(0, nullptr);
        }
    }

private:
    void load_and_replace_functions(const fs::path& obj_path) {
        if (DLL handle = DLL::open_deep(context.log, obj_path.c_str())) {
            std::string_view str_table = handle.string_table();
            for (size_t i = 0, l = str_table.size() - 1; i < l; ++i) {
                if (str_table[i] == '\0') {
                    const char* name = &str_table[i + 1];
                    void* func = handle.symbol(name);
                    // context.log.info(name);

                    if (func != nullptr && strlen(name) > 3) {
                        bool is_cpp = name[0] == '_' && name[1] == 'Z';
                        if (is_cpp) {
                            int error = plthook_replace(this->plthook, name, func, NULL);
                            (void)error;

                        // if (error == 0)
                            // context.log.error(error);
                        }
                    }
                }
            }

            this->loaded_dlls.emplace_back(std::move(handle));
        }
    }

    void update() {
        path_index = (path_index + 1) % files.size();

        SourceFile& file = files[path_index];
        if (file.type == SourceType::UNIT && file.has_source_changed()) {
            context.log.info(file.source_path, " changed!");
            // TODO: only recompile the actual function that has been changed.

            fs::path output_path = context.settings.output_dir / "tmp"
                    / ("tmp" + (std::to_string(this->temporary_files.size()) + ".so"));
            std::error_code ec;
            fs::create_directories(context.settings.output_dir / "tmp", ec);
            this->temporary_files.push_back(output_path);
            if (compile_file(context, file, output_path, true) == ErrorCode::OK) {
                load_and_replace_functions(output_path);
                context.log.info("Done!");
            }
            else {
                // Compilation failed, don't try again until the file changes again.
                file.compiled_time = file.source_time;
            }

            // TODO: try to rebuild the entire thing, but make it
            // cancelable so any further changes can also trigger a rebuild.
        }
    }
};


void run_tests(Context const& context) {
    // Open the created shared library.
    if (DLL dll = DLL::open_global(context.log, context.settings.output_file.c_str())) {
        typedef void (*Func)(void);
        std::vector<std::pair<const char*, Func>> test_functions;
        std::string_view str_table = dll.string_table();
        if (str_table.size() > 5)
            for (size_t i = 0, l = str_table.size() - 5; i < l; ++i)
                if (str_table[i] == '\0') {
                    const char* name = &str_table[++i];
                    if (str_table.substr(i).starts_with("__test_"sv)) {
                        i += 7;
                        Func func = (Func)dll.symbol(name);
                        if (func != nullptr)
                            test_functions.emplace_back(name, func);
                    }
                }

        context.log.info("Running ", test_functions.size(), " tests");
        context.log.set_task("TESTING", test_functions.size());
        ThreadPool pool(context.settings.job_count);
        for (auto [name, func]: test_functions)
            pool.enqueue([&context, func] -> ErrorCode {
                func();
                context.log.step_task();
                return ErrorCode::OK; // TODO: check for errors.
            });
        pool.join();
        context.log.clear_task();
        context.log.info("\n");
    }
}

static std::vector<std::string> get_arguments(int argn, char** argv) {
    std::vector<std::string> args;
    std::ifstream file_args("livecc.args");
    std::string line;
    while (std::getline(file_args, line)) {
        bool in_whitespace = true;
        bool quoted = false;
        uint start = 0;
        for (size_t i = 0, l = line.size(); i < l; ++i) {
            switch (line[i]) {
                case ' ': case '\t':
                    if (!quoted && !in_whitespace) {
                        in_whitespace = true;
                        args.emplace_back(line.substr(start, i - start));
                    }
                    break;
                case '\\': if (quoted) ++i; goto character;
                case '"': quoted = !quoted; goto character;
                default: character:
                    if (in_whitespace) {
                        in_whitespace = false;
                        start = i;
                    }
                    break;
            }
        }
        if (!in_whitespace)
            args.emplace_back(line.substr(start, line.size() - start));
    }

    for (int i = 1; i < argn; ++i)
        args.emplace_back(argv[i]);

    return args;
}

int main(int argn, char** argv) {
    std::error_code std_err;
    Context context;
    std::vector<InputFile> input;

    std::vector<std::string> args = get_arguments(argn, argv);
    ErrorCode err = parse_arguments(context, input, args);
    if (err != ErrorCode::OK) {
        context.log.error("failed parsing some arguments");
        // TODO: show help.
        return (int)err;
    }

    // Add the src directory by default if no sources have been specified.
    if (std::ranges::find_if(input, [] (InputFile& file) { return !file.type.is_include(); }) == input.end()) {
        add_source_directory(input, "src");
        if (input.empty()) {
            // TODO: show help.
            context.log.info("no input files");
            return (int)ErrorCode::NO_INPUT;
        }
    }

    if (context.settings.clean)
        fs::remove_all(context.settings.build_dir, std_err);

    err = get_system_include_dirs(context);
    if (err != ErrorCode::OK) return (int)err;

    DependencyTree dependency_tree;
    err = dependency_tree.build(context, input);
    if (err != ErrorCode::OK) return (int)err;

    update_compile_commands(context.settings, dependency_tree.files);

    if (dependency_tree.need_compilation() || !fs::exists(context.settings.output_file)) {
        if (!context.settings.do_compile)
            return 0;
        err = compile_and_link(context, dependency_tree.files);
        if (err != ErrorCode::OK) return (int)err;
    }

    if (context.settings.test)
        run_tests(context);
    else if (context.settings.build_type == BuildType::LIVE) {
        Runtime runtime{context, dependency_tree.files};
        runtime.run();
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