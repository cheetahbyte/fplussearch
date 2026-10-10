CXX ?= c++
PCRE2 := $(shell pkg-config --variable=prefix libpcre2-8)
CXXFLAGS ?= -std=c++20 -O3 -mcpu=native -Wall -Wextra -Wpedantic
CXXFLAGS += -isystem $(PCRE2)/include
LDFLAGS ?= -pthread -framework CoreServices
LDFLAGS += $(PCRE2)/lib/libpcre2-8.a

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)

LIBOBJ := $(filter-out build/main.o,$(OBJ))

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

build/test-search-matching: tests/test_search_matching.cpp $(LIBOBJ)
	$(CXX) $(CXXFLAGS) -Isrc $^ -o $@ $(LDFLAGS)

build/test-content: tests/test_content.cpp src/content.cpp $(wildcard src/*.hpp) $(filter-out build/content.o,$(LIBOBJ))
	$(CXX) $(CXXFLAGS) -Isrc $< $(filter-out build/content.o,$(LIBOBJ)) -o $@ $(LDFLAGS)

build/test-find-ci: tests/test_find_ci.cpp src/search.cpp $(wildcard src/*.hpp) $(filter-out build/search.o,$(LIBOBJ))
	$(CXX) $(CXXFLAGS) -Isrc $< $(filter-out build/search.o,$(LIBOBJ)) -o $@ $(LDFLAGS)

build/test-find-ci-scalar: tests/test_find_ci.cpp src/search.cpp $(wildcard src/*.hpp) $(filter-out build/search.o,$(LIBOBJ))
	$(CXX) $(CXXFLAGS) -U__ARM_NEON -Isrc $< $(filter-out build/search.o,$(LIBOBJ)) -o $@ $(LDFLAGS)

test-search: build/test-search-matching build/test-content build/test-find-ci build/test-find-ci-scalar
	./build/test-search-matching
	./build/test-content
	./build/test-find-ci
	./build/test-find-ci-scalar

.PHONY: clean lib test-search
