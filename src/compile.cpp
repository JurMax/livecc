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
    ThreadPool pool;

    void add_to_compile_queue(SourceFile& file) {
        if (file.need_compile) {
            pool.enqueue([&] -> bool {
                bool success = compile_file(context, file);
                if (success)
                    mark_compiled(file);
                context.log.step_task();
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

/** Return true if the given depends (indirectly) on the given dependency. Is slow */
static bool depends_on(SourceFile& file, SourceFile& dependency) {
    for (SourceFile* dependent : dependency.dependent_files) {
        if (dependent == &file) return true;
        else if (depends_on(file, *dependent)) return true;
    }
    return false;
}

static bool depends_on_print(SourceFile& file, SourceFile& dependency) {
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
        context.log.info(std::string_view{output});
    if (err != 0)
        file.compilation_failed = true;

    if (WIFSIGNALED(err) && (WTERMSIG(err) == SIGINT || WTERMSIG(err) == SIGQUIT))
        exit(1); // TODO: this is ugly, we have to shut down gracefully.

    if (file.compilation_failed) {
        fs::remove(output_path, ec);
        return false;
    }

    file.compiled_time = fs::last_write_time(output_path, ec);
    return true;
}

bool compile_file(const Context& context, SourceFile& file) {
    return compile_file(context, file, file.compiled_path, false);
}


bool compile_all(const Context& context, std::deque<SourceFile>& files) {
    size_t compile_count = std::ranges::count_if(files, [] (SourceFile& f) { return f.need_compile; });
    context.log.set_task("COMPILING", compile_count);
    Compiler compiler{context, context.job_count};

    // Add all files that have no dependencies to the compile queue.
    // As these compile they will add all the other files too.
    for (SourceFile& file : files)
        if (file.dependencies_count == 0)
            compiler.add_to_compile_queue(file);

    compiler.pool.join();
    context.log.clear_task();

    // Check for errors.
    int dependencies_missing = 0;
    int compilations_failed = 0;
    for (auto it = files.begin(), end = files.end(); it != end; ++it) {
        if (it->compilation_failed)
            compilations_failed++;
        else if (it->compiled_dependencies < it->dependencies_count)
            dependencies_missing++;
    }

    // Output errors.
    if (compilations_failed) {
        context.log.info();
        context.log.error("compilation failed for:");
        for (SourceFile& file : files)
            if (file.compilation_failed)
                std::cout << "        " << file.source_path << std::endl;
        return false;
    }
    else if (dependencies_missing) {
        context.log.info();
        context.log.error("files are missing one or more dependencies:");
        for (SourceFile& file : files)
            if (file.compiled_dependencies < file.dependencies_count)
                std::cout << "        " << file.source_path << std::endl;

        bool circular_dependencies = false;
        for (SourceFile& file : files) {
            if (file.compiled_dependencies < file.dependencies_count) {
                if (depends_on(file, file)) {
                    if (!circular_dependencies) {
                        circular_dependencies = true;
                        context.log.info();
                        context.log.error("circular dependencies found:");
                    }
                    std::cout << "        ";
                    depends_on_print(file, file);
                    std::cout << " -> " << file.source_path << std::endl;
                }
            }
        }
        return false;
    }

    return true;
}
