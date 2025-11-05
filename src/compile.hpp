#pragma once

#include "source_file.hpp"


// Returns true if no errors occurred.
bool compile_file(const Context& context, SourceFile& file,
                  const std::filesystem::path& output_path, bool live_compile);
bool compile_file(const Context& context, SourceFile& file);

bool compile_all(const Context& context, std::span<SourceFile> files);
