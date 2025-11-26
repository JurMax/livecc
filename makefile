SRC := $(wildcard src/*.cpp) $(wildcard src/**/*.cpp)
HEADERS := $(wildcard src/*.hpp) $(wildcard src/**/*.hpp)
OBJ := $(patsubst %.c, %.o, $(patsubst %.cpp, %.o, $(SRC)))

ARGS := -std=c++23 -g -Wall
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
	rm -r build
	rm src/*.o src/**/*.o livecc
