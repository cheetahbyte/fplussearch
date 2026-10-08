CXX ?= c++
PCRE2 := $(shell pkg-config --variable=prefix libpcre2-8)
CXXFLAGS ?= -std=c++20 -O3 -mcpu=native -Wall -Wextra -Wpedantic
CXXFLAGS += -isystem $(PCRE2)/include
LDFLAGS ?= -pthread -framework CoreServices
LDFLAGS += $(PCRE2)/lib/libpcre2-8.a

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)

LIBOBJ := $(filter-out build/main.o build/tui.o,$(OBJ))

build/fplussearch: $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

# The engine as a static library (link with -framework CoreServices and
# libpcre2-8); include src/live.hpp.
lib: build/libfplussearch.a

build/libfplussearch.a: $(LIBOBJ)
	rm -f $@
	ar rcs $@ $(LIBOBJ)

build/%.o: src/%.cpp $(wildcard src/*.hpp) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean lib
