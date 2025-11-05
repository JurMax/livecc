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
    const Context& context;
    std::span<SourceFile> files;
    std::span<std::atomic<int>> compiled_parents;
    std::span<char> failed;
    ThreadPool pool;

    void add_to_compile_queue(uint file) {
        if (files[file].need_compile) {
            pool.enqueue([this, file] -> bool {
                bool success = compile_file(context, files[file]);
                failed[file] = !success;
                if (success)
                    mark_compiled(files[file]);
                context.log.step_task();
                return !success;
            });
        }
        else // We don't need to compile this file, so add its dependencies to the queue.
            mark_compiled(files[file]);
    }

    void mark_compiled(SourceFile& file) {
        for (uint child : file.dependent_files)
            if (++compiled_parents[child] == files[child].parent_count)
                add_to_compile_queue(child);
    }
};

/** Return true if the given depends (indirectly) on the given dependency. Is slow */
static bool depends_on(std::span<SourceFile> files, uint file, uint dependency) {
    for (uint child : files[dependency].dependent_files) {
        if (child == file) return true;
        else if (depends_on(files, file, child)) return true;
    }
    return false;
}

static bool depends_on_print(std::span<SourceFile> files, uint file, uint dependency) {
    for (uint child : files[dependency].dependent_files) {
        if (child == file) {
            std::cout << files[child].source_path;
            return true;
        }
        else if (depends_on_print(files, file, child)) {
            std::cout << " -> " << files[child].source_path;
            return true;
        }
    }
    return false;
}


// Returns true if an error occurred. // TODO: move to compiler.cpp
bool compile_file(const Context& context, SourceFile& file, const std::filesystem::path& output_path, bool live_compile) {
    std::string build_command = file.get_build_command(context, output_path, live_compile);
    std::optional<ModuleMapperPipe> module_pipe;
    std::error_code ec;

    // Create PCH file.
    if (file.type == SourceFile::PCH) {
        if (context.compiler_type == Context::GCC)
            fs::copy_file(file.source_path, fs::path{file.pch_include()}, fs::copy_options::overwrite_existing, ec);
        else
            std::ofstream{fs::path{file.pch_include()}} << "#error PCH not included\n";
    }
    else if (context.compiler_type == Context::GCC && !file.uses_timestamp(context)) {
        module_pipe.emplace(context, file);
        build_command += module_pipe->mapper_arg();
    }

    if (context.verbose)
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
        do  {
            chars_read = fread(buff, 1, sizeof(buff), process);
            output.append_range(std::string_view{buff, chars_read});
        } while (chars_read == sizeof(buff));
    }

    // Check if the process was successful.
    int err = pclose(process);
    if (!output.empty() && (file.type != SourceFile::SYSTEM_HEADER || err != 0)) // Don't output system header warnings.
        context.log.info(/*"\e[1m", file.source_path.native(), ":\e[0m\n",*/ std::string_view{output}); // TODO: expand long lines with indentation.

    if (WIFSIGNALED(err) && (WTERMSIG(err) == SIGINT || WTERMSIG(err) == SIGQUIT))
        exit(1); // TODO: this is ugly, we have to shut down gracefully.

    if (err != 0) {
        fs::remove(output_path, ec);
        return false;
    }

    file.compiled_time = fs::last_write_time(output_path, ec);
    return true;
}

bool compile_file(const Context& context, SourceFile& file) {
    return compile_file(context, file, file.compiled_path, false);
}


bool compile_all(const Context& context, std::span<SourceFile> files) {
    size_t compile_count = std::ranges::count_if(files, [] (SourceFile& f) { return f.need_compile; });
    context.log.set_task("COMPILING", compile_count);

    std::vector<char> compilation_failed(files.size(), false);

    std::vector<std::atomic<int>> compiled_parents(files.size());
    for (auto& d : compiled_parents) std::atomic_init(&d, 0);

    // Compile everything.
    {
        Compiler compiler{context, files, compiled_parents, compilation_failed, context.job_count};

        // Add all files that have no dependencies to the compile queue.
        // As these compile they will add all the other files too.
        for (uint i : Range(files))
            if (files[i].parent_count == 0)
                compiler.add_to_compile_queue(i);

        compiler.pool.join();
        context.log.clear_task();
    }

    // Check for errors.
    {
        bool some_missing_dependencies = false;
        bool some_failed = false;
        for (uint i : Range(files)) {
            if (compilation_failed[i])
                some_failed = true;
            else if (compiled_parents[i] < files[i].parent_count)
                some_missing_dependencies = true;
        }

        // Output errors.
        if (some_failed) {
            context.log.info();
            context.log.error("compilation failed for:");
            for (uint i : Range(files))
                if (compilation_failed[i])
                    std::cout << "        " << files[i].source_path << std::endl;
            if (some_missing_dependencies) {
                std::cout << std::endl;
                for (uint i : Range(files))
                    if (compiled_parents[i] < files[i].parent_count)
                        std::cout << "        " << files[i].source_path << std::endl;
            }
            return false;
        }
        else if (some_missing_dependencies) {
            context.log.info();
            context.log.error("files are missing one or more dependencies:");
            for (uint i : Range(files))
                if (compiled_parents[i] < files[i].parent_count)
                    std::cout << "        " << files[i].source_path << std::endl;

            bool circular_dependencies = false;
            for (uint i : Range(files)) {
                if (compiled_parents[i] < files[i].parent_count) {
                    if (depends_on(files, i, i)) {
                        if (!circular_dependencies) {
                            circular_dependencies = true;
                            context.log.info();
                            context.log.error("circular dependencies found:");
                        }
                        std::cout << "        ";
                        depends_on_print(files, i, i);
                        std::cout << " -> " << files[i].source_path << std::endl;
                    }
                }
            }
            return false;
        }
    }

    return true;
}
