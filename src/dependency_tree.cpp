#include "dependency_tree.hpp"

#include <shared_mutex>
#include <unordered_map>

#include "thread_pool.hpp"

using namespace std::literals;

struct DependencyTreeBuilder {
    Context const& context;
    DependencyTree& tree;
    ThreadPool pool;
    std::unordered_map<fs::path, uint> source_map = {};

    // TODO: turn this into read/write thingies.
    std::shared_mutex files_mutex;
    std::mutex children_mutex;

    ErrorCode map_file_dependencies(uint file) {
        files_mutex.lock_shared();
        if (tree.files[file].read_dependencies(context) != ErrorCode::OK) {
            files_mutex.unlock_shared();
            return ErrorCode::FAILED;
        }

        // Mark the PCH as having zero dependencies, as it needs to be compiled first.
        if (tree.files[file].type.is_pch())
            tree.files[file].dependencies.clear();
        bool added_pch = false;

        // Get pointers to the dependencies so the list remains valid even if the files list moves.
        SourceFile::Dependency* dependency = &*tree.files[file].dependencies.begin();
        SourceFile::Dependency* dep_end = dependency + tree.files[file].dependencies.size();
        for (; dependency != dep_end; ++dependency) {
            uint header;
            {
                auto header_result = source_map.find(dependency->path);
                if (header_result != source_map.end())
                    header = header_result->second;
                else if (dependency->type == SourceType::MODULE) {
                    context.log.error("module [", dependency->path, "] imported in ", tree.files[file].source_path, " does not exist");
                    continue;
                }
                else {
                    // Get the write lock.
                    files_mutex.unlock_shared();
                    files_mutex.lock();

                    // Now that we have the write lock, check if some other file
                    // has not written the same header file in the meantime.
                    auto header_result = source_map.find(dependency->path);
                    if (header_result != source_map.end()) {
                        header = header_result->second;
                        files_mutex.unlock();
                    }
                    else {
                        // Insert the header as a new source file.
                        header = tree.files.size();
                        tree.files.emplace_back(context.settings, dependency->path, dependency->type);
                        source_map.emplace(dependency->path, header);
                        files_mutex.unlock();

                        context.log.increase_task_total();
                        pool.enqueue([this, header] -> ErrorCode {
                            return map_file_dependencies(header);
                        });
                    }

                    files_mutex.lock_shared();
                }
            }

            switch (tree.files[header].type) {
                case SourceType::PCH:
                case SourceType::C_PCH:
                    if (!added_pch) {
                        added_pch = true;
                        tree.files[file].build_includes.append_range("-include \""sv);
                        tree.files[file].build_includes.append_range(tree.files[header].pch_include());
                        tree.files[file].build_includes.append_range("\" "sv);
                    }
                    break;
                case SourceType::HEADER_UNIT:
                case SourceType::SYSTEM_HEADER_UNIT:
                    if (context.settings.compiler_type == Context::Settings::CLANG && tree.files[header].source_time) {
                        tree.files[file].build_includes.append_range("-fmodule-file=\""sv);
                        tree.files[file].build_includes.append_range(tree.files[header].compiled_path.native());
                        tree.files[file].build_includes.append_range("\" "sv);
                    }
                default: break;
            }

            tree.files[file].parents.push_back(header);
            children_mutex.lock();
            tree.files[header].children.push_back(file);
            children_mutex.unlock();
        }
        files_mutex.unlock_shared();

        context.log.step_task();
        return ErrorCode::OK;
    }
};

// Returns true on success.
ErrorCode DependencyTree::build(Context const& context, std::span<const InputFile> input) {
    for (InputFile const& in : input)
        files.emplace_back(context.settings, in.path, in.type);

    DependencyTreeBuilder builder{context, *this, context.settings.job_count};
    context.log.set_task("LOADING DEPENDENCIES", files.size());

    // Initialise the maps.
    for (uint file : Range(files)) {
        if (files[file].type == SourceType::MODULE) {
            auto it = builder.source_map.find(files[file].module_name);
            if (it == builder.source_map.end())
                builder.source_map.emplace(files[file].module_name, file);
            else {
                context.log.error("there are multiple implementations for module ", files[file].module_name,
                    "(in ", files[it->second].source_path, " and ", files[file].source_path, ")");
                builder.pool.got_error = true;
            }
        }
        else if (files[file].type.is_include())
            builder.source_map.emplace(files[file].source_path, file);

        // Make all files depend on the PCH
        if (files[file].type == SourceType::PCH)
            for (SourceFile& other : files)
                if (other.type.imports_modules())
                    other.dependencies.emplace_back(files[file].source_path, SourceType::PCH);
        if (files[file].type == SourceType::C_PCH)
            for (SourceFile& other : files)
                if (other.type == SourceType::C_UNIT)
                    other.dependencies.emplace_back(files[file].source_path, SourceType::C_PCH);
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
    //     context.log.print(file.source_path, " ", file.type, " ", has_changed, " ", !file.source_time, " ", !file.compiled_time, "\n");

    //     for (auto& parent : file.parents) {
    //         context.log.print("   ", parent.path, " ", parent.type, "\n");
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

static uint check_file_for_compilation(std::span<SourceFile> const& files, uint file) {
    visited_flag(files[file]) = true;
    if (!files[file].compiled_time
        || (files[file].source_time && *files[file].source_time > *files[file].compiled_time))
        return mark_file_for_compilation(files, file);
    else {
        uint compile_count = 0;
        for (uint child : files[file].children)
            if (!files[child].need_compile && !visited_flag(files[child]))
                compile_count += check_file_for_compilation(files, child);
        return compile_count;
    }
}

// Returns true if at least 1 file should be compiled.
bool DependencyTree::need_compilation() {
    uint compile_count = 0;
    for (SourceFile& file : files) {
        visited_flag(file) = false;
        file.need_compile = false;
    }
    for (uint i : Range(files))
        if (files[i].parents.size() == 0)
            compile_count += check_file_for_compilation(files, i);
    return compile_count != 0;
}
