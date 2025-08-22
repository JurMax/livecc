# Live C++

A C/C++ compiler front-end with fully automatic live/hot reload support.

## TODO

- Don't/fix system header modules compile warnings.
- Check if clang or livecc changed and recompile if so.
- Fix module support: actually build the two thingies.
- During live reload: build the normal object file on an extra thread, to allow quicker restart.
- Add a good help screen, with examples.
- Use inotify to check for file changes while running in live reload mode.
- Replace a lot of uses of fs::path and std::string to not allocate.
- Test what happens with missing files and what should happen
- Multiple PCH? --pch then applies to all files defined after it, or everything if only one is defined at all.
- Support C style pch

- More sensible default warning maybe?
