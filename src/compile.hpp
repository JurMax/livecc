#pragma once

#include "source_file.hpp"


ErrorCode compile_file(Context const& context, SourceFile& file,
                  const std::filesystem::path& output_path, bool live_compile);
ErrorCode compile_file(Context const& context, SourceFile& file);

ErrorCode compile_all(Context const& context, std::span<SourceFile> files);
