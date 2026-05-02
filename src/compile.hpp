#pragma once

#include "source_file.hpp"

namespace livecc {
    ErrorCode compile_all(Context const& context, std::span<SourceFile> files);

    ErrorCode compile_file(Context const& context, SourceFile& file,
                    const std::filesystem::path& output_path, bool live_recompile);
    ErrorCode compile_file(Context const& context, SourceFile& file);
}
