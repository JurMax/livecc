#pragma once

#include "source_file.hpp"

#include <unordered_map>

/**
 * Build the dependency tree
 */
class DependencyTree {
public:
    std::unordered_map<fs::path, uint> header_map;
    std::unordered_map<std::string, uint> module_map;

    /** Returns true on success. */
    bool build(const Context& context, std::vector<SourceFile>& files);

    /** Returns true if at least 1 file should be compiled. */
    uint mark_for_compilation(const Context& context, std::span<SourceFile> files);
};
