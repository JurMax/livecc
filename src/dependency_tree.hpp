#pragma once

#include "source_file.hpp"

#include <deque>
#include <unordered_map>

/**
 * Build the dependency tree
 */
class DependencyTree {
public:
    std::unordered_map<fs::path, SourceFile*> header_map;
    std::unordered_map<std::string, SourceFile*> module_map;

    /** Returns true on success. */
    bool build(const Context& context, std::deque<SourceFile>& files);

    /** Returns true if at least 1 file should be compiled. */
    uint mark_for_compilation(const Context& context, std::deque<SourceFile>& files);
};
