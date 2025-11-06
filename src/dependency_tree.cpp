#include "dependency_tree.hpp"
#include "thread_pool.hpp"
#include <shared_mutex>

using namespace std::literals;

struct DependencyTreeBuilder {
    Context const& context;
    std::vector<SourceFile>& files;
    DependencyTree& tree;
    ThreadPool pool;

    // TODO: turn this into read/write thingies.
    std::shared_mutex files_mutex;
    std::mutex children_mutex;

    ErrorCode map_file_dependencies(uint file) {
        files_mutex.lock_shared();
        if (files[file].read_dependencies(context) != ErrorCode::OK) {
            files_mutex.unlock_shared();
            return ErrorCode::FAILED;
        }

        // Mark the PCH as having zero dependencies, as it needs to be compiled first.
        if (files[file].type == SourceFile::PCH)
            files[file].parents.clear();
        bool added_pch = false;

        // Get pointers to the dependencies so the list remains valid even if the files list moves.
        SourceFile::Dependency* dependency = &files[file].parents[0u];
        SourceFile::Dependency* dep_end = dependency + files[file].parents.size();
        for (; dependency != dep_end; ++dependency) {
            uint header;
            {
                auto header_result = tree.source_map.find(dependency->path);
                if (header_result != tree.source_map.end())
                    header = header_result->second;
                else if (dependency->type == SourceFile::MODULE) {
                    context.log.error("module [", dependency->path, "] imported in ", files[file].source_path, " does not exist");
                    continue;
                }
                else {
                    // Get the write lock.
                    files_mutex.unlock_shared();
                    files_mutex.lock();

                    // Now that we have the write lock, check if some other file
                    // has not written the same header file in the meantime.
                    auto header_result = tree.source_map.find(dependency->path);
                    if (header_result != tree.source_map.end()) {
                        header = header_result->second;
                        files_mutex.unlock();
                    }
                    else {
                        // Insert the header as a new source file.
                        header = files.size();
                        files.emplace_back(context, dependency->path, dependency->type);
                        files[header].set_compile_path(context);
                        tree.source_map.emplace(dependency->path, header);
                        files_mutex.unlock();

                        context.log.increase_task_total();
                        pool.enqueue([this, header] -> ErrorCode {
                            return map_file_dependencies(header);
                        });
                    }

                    files_mutex.lock_shared();
                }
            }

            switch (files[header].type) {
                case SourceFile::PCH:
                    if (!added_pch) {
                        added_pch = true;
                        files[file].build_includes.append_range("-include \""sv);
                        files[file].build_includes.append_range(files[header].pch_include());
                        files[file].build_includes.append_range("\" "sv);
                    }
                    break;
                case SourceFile::HEADER_UNIT:
                case SourceFile::SYSTEM_HEADER_UNIT:
                    if (context.compiler_type == Context::CLANG) {
                        files[file].build_includes.append_range("-fmodule-file=\""sv);
                        files[file].build_includes.append_range(files[header].compiled_path.native());
                        files[file].build_includes.append_range("\" "sv);
                    }
                default: break;
            }

            children_mutex.lock();
            files[header].children.push_back(file);
            children_mutex.unlock();
        }

        context.log.step_task();
        files_mutex.unlock_shared();
        return ErrorCode::OK;
    }
};

// Returns true on success.
ErrorCode DependencyTree::build(Context const& context, std::vector<SourceFile>& files) {
    DependencyTreeBuilder builder{context, files, *this, context.job_count};

    context.log.set_task("LOADING DEPENDENCIES", files.size());

    // Initialise the maps.
    for (uint file : Range(files)) {
        if (files[file].type == SourceFile::MODULE) {
            auto it = source_map.find(files[file].module_name);
            if (it == source_map.end())
                source_map.emplace(files[file].module_name, file);
            else {
                context.log.error("there are multiple implementations for module ", files[file].module_name,
                    "(in ", files[it->second].source_path, " and ", files[file].source_path, ")");
                builder.pool.got_error = true;
            }
        }
        else if (files[file].is_include())
            source_map.emplace(files[file].source_path, file);

        // Make all files depend on the PCH // TODO: dont do this here so that we may support multiple pchs.
        if (files[file].type == SourceFile::PCH)
            for (SourceFile& other : files)
                if (!other.compile_to_timestamp())
                    other.parents.emplace_back(files[file].source_path, SourceFile::PCH);
    }

    // Read all the files. Store the size to avoid mapping a
    // header that was added later twice.
    for (uint i : Range(files))
        builder.pool.enqueue([&builder, i] -> ErrorCode {
            return builder.map_file_dependencies(i);
        });
    builder.pool.join();
    context.log.clear_task();

    // for (auto& file : files) {
    //     bool has_changed = !file.source_time || !file.compiled_time
    //         || *file.source_time > *file.compiled_time;
    //     std::cout << file.source_path << " " << file.type << " " << has_changed << " " << !file.source_time << " " << !file.compiled_time << std::endl;

    //     for (auto& parent : file.parents) {
    //         std::cout << "   " << parent.path << " " << parent.type << std::endl;
    //     }
    // }

    return builder.pool.got_error ? ErrorCode::FAILED : ErrorCode::OK;
}


static bool& visited_flag(SourceFile& file) { return file._temporary; }

static uint mark_file_for_compilation(std::span<SourceFile> const& files, uint file) {
    uint compile_count = 1;
    files[file].need_compile = true;
    for (uint child : files[file].children)
        if (!files[child].need_compile)
            compile_count += mark_file_for_compilation(files, child);
    return compile_count;
}

static uint check_file_for_compilation(Context const& context, std::span<SourceFile> const& files, uint file) {
    bool has_changed = !files[file].source_time || !files[file].compiled_time
        || *files[file].source_time > *files[file].compiled_time;
    visited_flag(files[file]) = true;
    if (!files[file].source_time)
        return 0; // File doesn't exist, which should have given a warning earlier already.
    else if (has_changed)
        return mark_file_for_compilation(files, file);
    else {
        uint compile_count = 0;
        for (uint child : files[file].children)
            if (!files[child].need_compile && !visited_flag(files[child]))
                compile_count += check_file_for_compilation(context, files, child);
        return compile_count;
    }
}

// Returns true if at least 1 file should be compiled.
uint DependencyTree::mark_for_compilation(Context const& context, std::span<SourceFile> files) {
    uint compile_count = 0;
    for (SourceFile& file : files) {
        visited_flag(file) = false;
        file.need_compile = false;
    }
    for (uint i : Range(files))
        if (files[i].parents.size() == 0)
            compile_count += check_file_for_compilation(context, files, i);
    return compile_count;
}
