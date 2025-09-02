#include "context.hpp"

#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

Context::Logging::Logging() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    term_width = w.ws_col;
}

void Context::Logging::set_task(const std::string_view& task, int task_total) {
    std::unique_lock<std::mutex> lock(mutex);
    task_name = task;
    bar_task_total = task_total;
    bar_task_current = 0;
}

void Context::Logging::increase_task_total(int amount) {
    std::unique_lock<std::mutex> lock(mutex);
    bar_task_total++;
}

void Context::Logging::clear_task() {
    std::unique_lock<std::mutex> lock(mutex);
    task_name.clear();
}

void Context::Logging::step_task() {
    std::unique_lock<std::mutex> lock(mutex);
    ++bar_task_current;
    print_bar();
}

// Set the full width of the terminal to whitespace
void Context::Logging::clear_term() const {
    for (int i = 0; i < term_width; ++i)
        std::cout << ' ';
    std::cout << '\r';
}

void Context::Logging::print_bar() const {
    if (!task_name.empty()) {
        std::cout << task_name << " [";
        int length = term_width - task_name.length() - 2 - 7;
        int progress = bar_task_current * length / bar_task_total;
        int i = 0;
        for (; i < progress; ++i) std::cout << '=';
        if (i < length) std::cout << '>';
        for (++i; i < length; ++i) std::cout << ' ';
        std::cout << std::format("] {:>3}%\r", bar_task_current * 100 / bar_task_total);
    }

    std::cout.flush();
}
