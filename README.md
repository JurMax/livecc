# Live C++

A C/C++ compiler front-end with fully automatic live/hot reload support.

## TODO

- Use header units/modules instead of PCH
- Default to C++23, test if mold exists and use that, use C whatever, allow overrides.
- Better support for C compilation / support for C files based on extension.
- Create a way to see if the arguments have changed, and everything needs to be recompiled
- Creating a compile_commands.json
- Startup error if clang doesnt exist.
- During live reload: build the normal object file on an extra thread, to allow quicker restart.
- Add a good help screen, with examples.
- Use inotify to check for file changes while running in live reload mode.
- Add a better frontend: livecc, livecc run, livecc build, livecc shared, livecc debug
- Replace a lot of uses of fs::path and std::string to not allocate.
