#include "module_mapper_pipe.hpp"

#include <ranges>
#include <string_view>

#include "context.hpp"
#include "source_file.hpp"

using namespace std::literals;

static constexpr inline ulong hash(auto iterable) {
    // Source: http://www.cse.yorku.ca/~oz/hash.html
    ulong hash = 5381;
    for (auto c : iterable) /* hash * 33 + c */
        hash = ((hash << 5) + hash) + (ulong)c;
    return hash;
}


// SOURCES:
// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p1184r2.pdf
// https://github.com/urnathan/libcody
static void thread_func(Context const* context, SourceFile* file, int input_fd, int output_fd) {
    char buffer[8192];
    constexpr auto as_string = std::views::transform([] (auto i) { return std::string_view{i}; });
    const auto write = [&] (auto... view) {
        return ((::write(output_fd, view.data(), view.length()) != -1) && ...);
    };

    while (true) {
        ssize_t num_read = read(input_fd, buffer, sizeof(buffer));
        if (num_read <= 0) return;

        bool is_batch = false; // Ignore final newline ------------vvv
        for (auto line : std::string_view{buffer, (size_t)num_read - 1} | std::views::split(" ;\n"sv) | as_string) {
            if (is_batch && !write(" ;\n"sv))
                return;
            is_batch = true;
            auto args = line | std::views::split(' ') | as_string;
            auto args_it = args.begin(), args_end = args.end();
            if (context->settings.verbose)
                context->log.info("GOT INPUT: ", line);
            switch (hash(*args_it++)) {
                case hash("HELLO"sv):
                    if (!write("HELLO 1 LIVECC"sv))
                        return;
                    break;
                case hash("MODULE-REPO"sv):
                    if (!write("MODULE-REPO \""sv, context->settings.output_directory.native(), "/module_repo\""sv))
                        return;
                    break;
                case hash("MODULE-EXPORT"sv):
                    if (args_it == args_end) goto err;
                    if (*args_it != file->module_name)
                        context->log.error("module names dont match: got ", *args_it, " but expected ", file->module_name);
                    if (!write("PATHNAME \""sv, file->compiled_path.native(), "\""sv))
                        return;
                    break;
                case hash("MODULE-COMPILED"sv):
                    if (args_it == args_end) goto err;
                    // TODO: start compilations depending on this module.
                    if (!write("OK"sv))
                        return;
                    break;
                case hash("MODULE-IMPORT"sv):
                    if (args_it == args_end) goto err;
                    // TODO
                    context->log.error("not implemented: ", line);
                    if (!write("ERROR NOT_IMPLEMENTED"sv))
                        return;
                    break;
                case hash("INCLUDE-TRANSLATE"sv):
                    if (args_it == args_end) goto err;
                    // TODO: return pathname to header modules.
                    if (!write("BOOL TRUE"sv))
                        return;
                    break;
                case hash("INVOKE"sv):
                    context->log.error("request not supported: ", line);
                    if (!write("ERROR NOT_SUPPORTED"sv))
                        return;
                    break;
                default: err:
                    context->log.error("invalid request: ", line);
                    if (!write("ERROR INVALID_REQUEST"sv))
                        return;
                    break;
            }
        }

        if (!write("\n"sv))
            return;
    }
}


ModuleMapperPipe::ModuleMapperPipe(Context const& context, SourceFile& file) {
    pipe(input_pipe);
    pipe(output_pipe);
    thread = std::thread(thread_func, &context, &file, input_pipe[0], output_pipe[1]);
}

ModuleMapperPipe::~ModuleMapperPipe() {
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    // Thread gets closed automatically.
    thread.join();
}

std::string ModuleMapperPipe::mapper_arg() {
    return std::format(" -fmodule-mapper=\"<{}>{}\"", output_pipe[0], input_pipe[1]);
}
