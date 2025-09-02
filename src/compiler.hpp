#pragma once

#include "source_file.hpp"
#include <deque>

bool parallel_compile_files(Context& context, std::deque<SourceFile>& files, size_t compile_count);
