SRC := src/livecc.cpp src/source_file.cpp src/plthook/plthook_elf.c
OBJ := $(patsubst %.c, %.o, $(patsubst %.cpp, %.o, $(SRC)))

CC := g++ -std=c++23 -Wall -O3

%.o: %.cpp
	${CC} -c $< -o $@

%.o: %.c
	${CC} -c $< -o $@

main: $(OBJ)
	${CC} $(OBJ) -o livecc
