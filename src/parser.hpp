#pragma once

#include <set>
#include <filesystem>

namespace fs = std::filesystem;


void get_file_includes( const fs::path& source_file,
                        std::set<std::string> defines,
                        std::set<fs::path>& out_includes,
                        std::set<std::string>& out_modules ); // TODO.