#pragma once

#include "source_file.hpp"


// Returns true if no errors occurred.
bool compile_file(Context const& context, SourceFile& file,
                  const std::filesystem::path& output_path, bool live_compile);
bool compile_file(Context const& context, SourceFile& file);

bool compile_all(Context const& context, std::span<SourceFile> files);
