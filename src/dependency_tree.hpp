#pragma once

#include "source_file.hpp"

#include <unordered_map>

/**
 * Build the dependency tree
 */
class DependencyTree {
public:

    // File or identifier mapped to a source_file index.
    std::unordered_map<fs::path, uint> source_map;

    /** Returns true on success. */
    bool build(Context const& context, std::vector<SourceFile>& files);

    /** Returns true if at least 1 file should be compiled. */
    uint mark_for_compilation(Context const& context, std::span<SourceFile> files);
};
