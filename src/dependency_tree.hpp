#pragma once

#include "source_file.hpp"

/**
 * Build the dependency tree
 */
class DependencyTree {
public:
    // File or identifier mapped to a source_file index.
    std::vector<SourceFile> files;

    /** Build the dependency tree inside of the files. */
    ErrorCode build(Context const& context, std::span<const InputFile> files);

    /** Returns true if at least 1 file should be compiled. */
    bool need_compilation();
};
