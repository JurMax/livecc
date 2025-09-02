#include "compiler.hpp"
#include "thread_pool.hpp"


/**
 * Compile te files that need to be compiled.
 */
struct Compiler {
    Context& context;
    ThreadPool pool;

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


bool parallel_compile_files(Context& context, std::deque<SourceFile>& files, size_t compile_count) {
    context.log_set_task("COMPILING", compile_count);
    Compiler compiler{context, context.job_count};

    // Add all files that have no dependencies to the compile queue.
    // As these compile they will add all the other files too.
    for (SourceFile& file : files)
        if (file.dependencies_count == 0)
            compiler.add_to_compile_queue(file);

    compiler.pool.join();
    context.log_clear_task();

    int dependencies_missing = 0;
    int compilations_failed = 0;
    for (auto it = files.begin(), end = files.end(); it != end; ++it) {
        if (it->compilation_failed)
            compilations_failed++;
        else if (it->compiled_dependencies < it->dependencies_count)
            dependencies_missing++;
    }

    if (compilations_failed) {
        context.log_info();
        context.log_error("compilation failed for:");
        for (SourceFile& file : files)
            if (file.compilation_failed)
                std::cout << "        " << file.source_path << std::endl;
        return false;
    }
    else if (dependencies_missing) {
        context.log_info();
        context.log_error("files are missing one or more dependencies:");
        for (SourceFile& file : files)
            if (file.compiled_dependencies < file.dependencies_count)
                std::cout << "        " << file.source_path << std::endl;

        bool circular_dependencies = false;
        for (SourceFile& file : files) {
            if (file.compiled_dependencies < file.dependencies_count) {
                if (depends_on(file, file)) {
                    if (!circular_dependencies) {
                        circular_dependencies = true;
                        context.log_info();
                        context.log_error("circular dependencies found:");
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
