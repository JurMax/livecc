#include "dependency_tree.hpp"
#include "thread_pool.hpp"

using namespace std::literals;

struct DependencyTreeBuilder {
    const Context& context;
    std::deque<SourceFile>& files;
    DependencyTree& tree;
    ThreadPool pool;
    std::mutex header_mutex;
    std::mutex module_mutex;

    bool map_file_dependencies(SourceFile& file) {
        file.read_dependencies(context);

        if (!file.include_dependencies.empty()) {
            std::lock_guard<std::mutex> lock(header_mutex);
            for (const auto& [header_path, type] : file.include_dependencies) {
                auto it = tree.header_map.find(header_path);
                if (it == tree.header_map.end()) {
                    // Insert the header as a new source file.
                    SourceFile& header = files.emplace_back(context, header_path, type);
                    header.set_compile_path(context);
                    it = tree.header_map.emplace(header_path, &header).first;
                    context.log.increase_task_total();
                    pool.enqueue([&] -> bool {
                        return map_file_dependencies(header);
                    });
                }

                SourceFile::type_t actual_type = it->second->type;

                if (actual_type == SourceFile::PCH && actual_type != SourceFile::SYSTEM_HEADER) {
                    file.build_includes.append_range("-include \""sv);
                    file.build_includes.append_range(it->second->pch_include());
                    file.build_includes.append_range("\" "sv);
                }
                else if (context.use_header_units
                    && (actual_type == SourceFile::HEADER || actual_type == SourceFile::SYSTEM_HEADER)) {
                    if (context.compiler_type == Context::CLANG) {
                        file.build_includes.append_range("-fmodule-file=\""sv);
                        file.build_includes.append_range(it->second->compiled_path.native());
                        file.build_includes.append_range("\" "sv);
                    }
                }

                it->second->dependent_files.push_back(&file);
                file.dependencies_count++;
            }
        }

        if (!file.module_dependencies.empty()) {
            std::lock_guard<std::mutex> lock(module_mutex);
            for (const std::string& module : file.module_dependencies) {
                auto it = tree.module_map.find(module);
                if (it != tree.module_map.end()) {
                    it->second->dependent_files.push_back(&file);
                    file.dependencies_count++;
                }
                else {
                    context.log.error("module [", module, "] imported in ", file.source_path, " does not exist");
                }
            }
        }

        // Mark the PCH as having zero dependencies, as it needs to be compiled first.
        if (file.type == SourceFile::PCH)
            file.dependencies_count = 0;

        context.log.step_task();
        return false;
    }
};

// Returns true on success.
bool DependencyTree::build(const Context& context, std::deque<SourceFile>& files) {
    DependencyTreeBuilder builder{context, files, *this, context.job_count};

    context.log.set_task("LOADING DEPENDENCIES", files.size());

    // Initialise the maps.
    for (SourceFile& f : files) {
        if (f.type == SourceFile::MODULE) {
            auto it = module_map.find(f.module_name);
            if (it == module_map.end())
                module_map.emplace(f.module_name, &f);
            else {
                context.log.error("there are multiple implementations for module ", f.module_name,
                    "(in ", it->second->source_path, " and ", f.source_path, ")");
                return false;
            }
        }
        else if (f.is_include())
            header_map.emplace(f.source_path, &f);

        // Make all files depend on the PCH
        if (f.type == SourceFile::PCH)
            for (SourceFile& i : files)
                if (i.type == SourceFile::UNIT || i.type == SourceFile::MODULE || i.type == SourceFile::HEADER)
                    i.include_dependencies.insert_or_assign(f.source_path, SourceFile::PCH);
    }

    // Read all the files. Store the size to avoid mapping a
    // header that was added later twice.
    for (size_t i = 0, l = files.size(); i != l; ++i)
        builder.pool.enqueue([&, &file = files[i]] {
            return builder.map_file_dependencies(file);
        });
    builder.pool.join();
    context.log.clear_task();
    return true;
}


static uint mark_file_for_compilation(SourceFile& file) {
    uint compile_count = 1;
    file.need_compile = true;
    file.visited = true;
    for (SourceFile* child : file.dependent_files)
        if (!child->need_compile)
            compile_count += mark_file_for_compilation(*child);
    return compile_count;
}

static uint check_file_for_compilation(SourceFile& file) {
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

// Returns true if at least 1 file should be compiled.
uint DependencyTree::mark_for_compilation(const Context& context, std::deque<SourceFile>& files) {
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
