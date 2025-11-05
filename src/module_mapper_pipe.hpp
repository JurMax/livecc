#pragma once

#include <thread>
#include <string>

struct Context;
class SourceFile;

class ModuleMapperPipe {
    std::thread thread;
    int input_pipe[2];
    int output_pipe[2];

public:
    ModuleMapperPipe(const Context& context, SourceFile& file);
    ~ModuleMapperPipe();

    std::string mapper_arg();
};

