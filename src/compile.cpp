#include "compile.hpp"
#include "module_mapper_pipe.hpp"
#include "thread_pool.hpp"

#include <algorithm>
#include <csignal>
#include <fstream>


/**
 * Compile te files that need to be compiled.
 */
struct Compiler {
    struct FileInfo {
        bool failed = false;
        std::atomic<int> compiled_parents = 0;
    };

    Context const& context;
    std::span<SourceFile> files;
    std::span<FileInfo> info;
    ThreadPool pool;

    void add_to_compile_queue(uint file) {
        if (files[file].need_compile) {
            pool.enqueue([this, file] -> ErrorCode {
                ErrorCode err = compile_file(context, files[file]);
                if (err == ErrorCode::OK)
                    mark_compiled(files[file]);
                else
                    info[file].failed = true;
                if (!files[file].type.compile_to_timestamp())
                    context.log.step_task();
                return err;
            });
        }
        else // We don't need to compile this file, so add its dependencies to the queue.
            mark_compiled(files[file]);
    }

    void mark_compiled(SourceFile& file) {
        for (uint child : file.children)
            if (++info[child].compiled_parents == files[child].parents.size())
                add_to_compile_queue(child);
    }
};

/** Return true if the given depends (indirectly) on the given dependency. Is slow */
static bool depends_on(std::span<const SourceFile> files, uint file, uint dependency) {
    for (uint child : files[dependency].children) {
        if (child == file) return true;
        else if (depends_on(files, file, child)) return true;
    }
    return false;
}

static bool depends_on_print(Context::Logging& log, std::span<const SourceFile> files, uint file, uint dependency) {
    for (uint child : files[dependency].children) {
        if (child == file) {
            log.print(files[child].source_path);
            return true;
        }
        else if (depends_on_print(log, files, file, child)) {
            log.print(" -> ", files[child].source_path);
            return true;
        }
    }
    return false;
}

ErrorCode compile_all(Context const& context, std::span<SourceFile> files) {
    size_t compile_count = std::ranges::count_if(files, [] (SourceFile& f) {
        return f.need_compile && !f.type.compile_to_timestamp();
    });
    context.log.set_task("COMPILING", compile_count);

    std::vector<Compiler::FileInfo> info(files.size());

    // Compile everything.
    {
        Compiler compiler{context, files, info, context.settings.job_count};

        // Add all files that have no dependencies to the compile queue.
        // As these compile they will add all the other files too.
        for (uint i : Range(files))
            if (files[i].parents.size() == 0)
                compiler.add_to_compile_queue(i);

        compiler.pool.join();
        context.log.clear_task();
    }

    // Check for errors.
    {
        bool some_missing_dependencies = false;
        bool some_failed = false;
        for (uint i : Range(files)) {
            if (info[i].failed)
                some_failed = true;
            else if (info[i].compiled_parents < files[i].parents.size())
                some_missing_dependencies = true;
        }

        // Output errors.
        if (some_failed) {
            context.log.info();
            context.log.error("compilation failed for:");
            for (uint i : Range(files))
                if (info[i].failed)
                    context.log.print("        ", files[i].source_path, "\n");
            if (some_missing_dependencies) {
                context.log.print("\n");
                for (uint i : Range(files))
                    if (info[i].compiled_parents < files[i].parents.size())
                        context.log.print("        ", files[i].source_path, "\n");
            }
            return ErrorCode::FAILED;
        }
        else if (some_missing_dependencies) {
            context.log.info();
            context.log.error("files are missing one or more dependencies:");
            for (uint i : Range(files))
                if (info[i].compiled_parents < files[i].parents.size())
                    context.log.print("        ", files[i].source_path, "\n");

            bool circular_dependencies = false;
            for (uint i : Range(files)) {
                if (info[i].compiled_parents < files[i].parents.size()) {
                    if (depends_on(files, i, i)) {
                        if (!circular_dependencies) {
                            circular_dependencies = true;
                            context.log.info();
                            context.log.error("circular dependencies found:");
                        }
                        context.log.print("        ");
                        depends_on_print(context.log, files, i, i);
                        context.log.print(" -> ", files[i].source_path, "\n");
                    }
                }
            }
            return ErrorCode::FAILED;
        }
    }

    return ErrorCode::OK;
}


ErrorCode compile_file(Context const& context, SourceFile& file, fs::path const& output_path, bool live_compile) {
    // If the output is only a timestamp, just update it.
    if (file.type.compile_to_timestamp()) {
        file.compiled_time = file.source_time;
        std::ofstream stream(output_path);
        return stream.is_open() ? ErrorCode::OK : ErrorCode::OPEN_FAILED;
    }

    std::string build_command = file.get_build_command(context.settings, output_path, live_compile);
    std::optional<ModuleMapperPipe> module_pipe;
    std::error_code ec;

    // Create PCH file.
    if (file.type == SourceType::PCH) {
        if (context.settings.compiler_type == Context::Settings::GCC)
            fs::copy_file(file.source_path, fs::path{file.pch_include()}, fs::copy_options::overwrite_existing, ec);
        else
            std::ofstream{fs::path{file.pch_include()}} << "#error PCH not included\n";
    }
    else if (context.settings.compiler_type == Context::Settings::GCC) {
        module_pipe.emplace(context, file);
        build_command += module_pipe->mapper_arg();
    }

    if (context.settings.verbose)
        context.log.info("Compiling ", file.source_path, " to ", output_path, " using: ", build_command);
    else
        context.log.info("Compiling ", file.source_path, " to ", output_path);

    build_command += " 2>&1";

    // Run the command and capture its output.
    FILE* process = popen(build_command.c_str(), "r");
    std::vector<char> output;
    {
        char buff[256];
        size_t chars_read;
        do {
            chars_read = fread(buff, 1, sizeof(buff), process);
            output.append_range(std::string_view{buff, chars_read});
        } while (chars_read == sizeof(buff));
    }

    // Check if the process was successful.
    int err = pclose(process);
    if (!output.empty())
        switch (file.type) {
            // Don't output system header warnings.
            case SourceType::SYSTEM_HEADER: case SourceType::SYSTEM_HEADER_UNIT:
                if (err == 0) break;
                [[fallthrough]];
            default:
                context.log.info(/*"\e[1m", file.source_path.native(), ":\e[0m\n",*/ std::string_view{output}); // TODO: expand long lines with indentation.
                break;
        }

    if (WIFSIGNALED(err) && (WTERMSIG(err) == SIGINT || WTERMSIG(err) == SIGQUIT))
        exit(1); // TODO: this is ugly, we have to shut down gracefully.

    if (err != 0) {
        fs::remove(output_path, ec);
        file.compiled_time = file.source_time;
        return ErrorCode::FAILED;
    }
    else {
        file.compiled_time = fs::last_write_time(file.source_path, ec);
        return ErrorCode::OK;
    }
}

ErrorCode compile_file(Context const& context, SourceFile& file) {
    return compile_file(context, file, file.compiled_path, false);
}
