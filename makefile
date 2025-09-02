SRC := $(wildcard src/*.cpp) src/plthook/plthook_elf.c
HEADERS := $(wildcard src/*.hpp)
OBJ := $(patsubst %.c, %.o, $(patsubst %.cpp, %.o, $(SRC)))

ARGS := -std=c++23 -Wall -O3
CC := clang++ ${ARGS}

everything:
	make main -j 16

%.o: %.cpp ${HEADERS}
	${CC} -c $< -o $@

%.o: %.c
	${CC} -c $< -o $@

main: $(OBJ)
	${CC} $(OBJ) -o livecc

test: main
	./livecc ${ARGS} -lstdc++ src

db: main
	gdb -ex run --args ./livecc src

clean:
	rm src/*.o livecc