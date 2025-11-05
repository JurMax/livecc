#include "dependency_tree.hpp"
#include "thread_pool.hpp"
#include <shared_mutex>

using namespace std::literals;

struct DependencyTreeBuilder {
    const Context& context;
    std::vector<SourceFile>& files;
    DependencyTree& tree;
    ThreadPool pool;

    // TODO: turn this into read/write thingies.
    std::shared_mutex files_mutex;
    std::mutex dependent_files_mutex;

    bool map_file_dependencies(uint file) {
        files_mutex.lock_shared();
        files[file].read_dependencies(context);
        bool added_pch = false;

        SourceFile::Dependency* dependency = &files[file].include_dependencies[0u];
        SourceFile::Dependency* end = dependency + files[file].include_dependencies.size();
        for (; dependency != end; ++dependency) {
            uint header;
            {
                auto header_result = tree.header_map.find(dependency->path);
                if (header_result != tree.header_map.end())
                    header = header_result->second;
                else {
                    files_mutex.unlock_shared();
                    files_mutex.lock();
                    {
                        // Insert the header as a new source file.
                        header = files.size();
                        files.emplace_back(context, dependency->path, dependency->type);
                        files[header].set_compile_path(context);
                        tree.header_map.emplace(dependency->path, header);
                    }
                    files_mutex.unlock();

                    context.log.increase_task_total();
                    pool.enqueue([this, header] -> bool {
                        return map_file_dependencies(header);
                    });

                    files_mutex.lock_shared();
                }
            }

            SourceFile::type_t header_type = files[header].type;

            if (header_type == SourceFile::PCH && header_type != SourceFile::SYSTEM_HEADER) {
                if (!added_pch) {
                    added_pch = true;
                    files[file].build_includes.append_range("-include \""sv);
                    files[file].build_includes.append_range(files[header].pch_include());
                    files[file].build_includes.append_range("\" "sv);
                }
            }
            else if (context.use_header_units
                && (header_type == SourceFile::HEADER || header_type == SourceFile::SYSTEM_HEADER)) {
                if (context.compiler_type == Context::CLANG) {
                    files[file].build_includes.append_range("-fmodule-file=\""sv);
                    files[file].build_includes.append_range(files[header].compiled_path.native());
                    files[file].build_includes.append_range("\" "sv);
                }
            }

            dependent_files_mutex.lock();
            files[header].dependent_files.push_back(file);
            dependent_files_mutex.unlock();
            files[file].parent_count++;
        }

        for (const std::string& module : files[file].module_dependencies) {
            auto it = tree.module_map.find(module);
            if (it != tree.module_map.end()) {
                dependent_files_mutex.lock();
                files[it->second].dependent_files.push_back(file);
                dependent_files_mutex.unlock();
                files[file].parent_count++;
            }
            else {
                context.log.error("module [", module, "] imported in ", files[file].source_path, " does not exist");
            }
        }

        // Mark the PCH as having zero dependencies, as it needs to be compiled first.
        if (files[file].type == SourceFile::PCH)
            files[file].parent_count = 0;

        context.log.step_task();
        files_mutex.unlock_shared();
        return false;
    }
};

// Returns true on success.
bool DependencyTree::build(const Context& context, std::vector<SourceFile>& files) {
    DependencyTreeBuilder builder{context, files, *this, context.job_count};

    context.log.set_task("LOADING DEPENDENCIES", files.size());

    // Initialise the maps.
    for (uint file : Range(files)) {
        if (files[file].type == SourceFile::MODULE) {
            auto it = module_map.find(files[file].module_name);
            if (it == module_map.end())
                module_map.emplace(files[file].module_name, file);
            else {
                context.log.error("there are multiple implementations for module ", files[file].module_name,
                    "(in ", files[it->second].source_path, " and ", files[file].source_path, ")");
                return false;
            }
        }
        else if (files[file].is_include())
            header_map.emplace(files[file].source_path, file);

        // Make all files depend on the PCH
        if (files[file].type == SourceFile::PCH)
            for (SourceFile& other : files)
                if (other.type == SourceFile::UNIT || other.type == SourceFile::MODULE || other.type == SourceFile::HEADER)
                    other.include_dependencies.emplace_back(files[file].source_path, SourceFile::PCH);
    }

    // Read all the files. Store the size to avoid mapping a
    // header that was added later twice.
    for (uint i : Range(files))
        builder.pool.enqueue([&builder, i] {
            return builder.map_file_dependencies(i);
        });
    builder.pool.join();
    context.log.clear_task();
    return true;
}


static uint mark_file_for_compilation(std::span<SourceFile> const& files, uint file) {
    uint compile_count = 1;
    files[file].need_compile = true;
    files[file].visited = true;
    for (uint child : files[file].dependent_files)
        if (!files[child].need_compile)
            compile_count += mark_file_for_compilation(files, child);
    return compile_count;
}

static uint check_file_for_compilation(std::span<SourceFile> const& files, uint file) {
    if (files[file].has_changed)
        return mark_file_for_compilation(files, file);
    else {
        files[file].visited = true;
        uint compile_count = 0;
        for (uint child : files[file].dependent_files)
            if (!files[child].visited)
                compile_count += check_file_for_compilation(files, child);
        return compile_count;
    }
}

// Returns true if at least 1 file should be compiled.
uint DependencyTree::mark_for_compilation(const Context& context, std::span<SourceFile> files) {
    uint compile_count = 0;
    if (context.build_command_changed) {
        for (SourceFile& file : files)
            file.need_compile = true;
        compile_count += files.size();
    }
    else {
        for (SourceFile& file : files) {
            file.need_compile = false;
            file.visited = false;
        }
        for (uint i : Range(files))
            if (files[i].parent_count == 0)
                compile_count += check_file_for_compilation(files, i);
    }
    return compile_count;
}
