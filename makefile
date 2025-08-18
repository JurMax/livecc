SRC := src/livecc.cpp src/source_file.cpp src/plthook/plthook_elf.c
OBJ := $(patsubst %.c, %.o, $(patsubst %.cpp, %.o, $(SRC)))

CC := clang++ -std=c++23 -Wall -O3

%.o: %.cpp src/globals.hpp src/source_file.hpp
	${CC} -c $< -o $@

%.o: %.c
	${CC} -c $< -o $@

main: $(OBJ)
	${CC} $(OBJ) -o livecc

clean:
	rm src/*.o livecc