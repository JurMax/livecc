# Live C++

A C/C++ compiler front-end with fully automatic live/hot reload support.

## TODO

- Use the same canonical paths as GCC does in the header/module map, and use , instead of resolving it.
- Support for importing header files -> merge the header file and the module file somehow:
    - If --use-header-units is not used, the same file needs to be able to be used as an header if included and as a module if explicitly imported. (prob just treat it as a module header file, doesnt matter if some files have to wait a bit longer before its ready)


- Imported headers should become header units automatically (maybe add two versions?)
    - we can see if they're header unist based on "" / <>
- Check if clang or livecc changed and recompile if so.
- Fix module support: actually build the two thingies.
- During live reload: build the normal object file on an extra thread, to allow quicker restart.
- Add a good help screen, with examples.
- Use inotify to check for file changes while running in live reload mode.
- Replace a lot of uses of fs::path and std::string to not allocate.
- Test what happens with missing files and what should happen
- Multiple PCH? --pch then applies to all files defined after it, or everything if only one is defined at all.
- Support C style pch
- Allow changing the compiler from clang to other things like gcc.
- GCC support by adding an support for -fmodule-mapper: https://gcc.gnu.org/wiki/cxx-modules#CMI_Location.

- More sensible default warning maybe?
