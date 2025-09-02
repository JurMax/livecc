#pragma once

#include "source_file.hpp"
#include <deque>


// Returns true if no errors occurred.
bool compile_file(const Context& context, SourceFile& file,
                  const std::filesystem::path& output_path, bool live_compile);
bool compile_file(const Context& context, SourceFile& file);

bool compile_all(const Context& context, std::deque<SourceFile>& files);
