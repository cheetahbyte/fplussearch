CXX ?= c++
PCRE2 := $(shell pkg-config --variable=prefix libpcre2-8)
CXXFLAGS ?= -std=c++20 -O3 -mcpu=native -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc -isystem $(PCRE2)/include
LDFLAGS ?= -pthread -framework CoreServices
LDFLAGS += $(PCRE2)/lib/libpcre2-8.a

SRC := $(shell find src -name '*.cpp' | sort)
HEADERS := $(shell find src -name '*.hpp' | sort)
OBJ := $(SRC:src/%.cpp=build/%.o)

LIBOBJ := $(filter-out build/main.o build/cli/%,$(OBJ))

build/fplussearch: $(OBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS)

# The engine as a static library (link with -framework CoreServices and
# libpcre2-8); include live/live.hpp with -Isrc.
lib: build/libfplussearch.a

build/libfplussearch.a: $(LIBOBJ)
	rm -f $@
	ar rcs $@ $(LIBOBJ)

build/%.o: src/%.cpp | build
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

build/test-search-matching: tests/test_search_matching.cpp $(HEADERS) $(LIBOBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBOBJ) -o $@ $(LDFLAGS)

build/test-content: tests/test_content.cpp $(HEADERS) $(LIBOBJ)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LIBOBJ) -o $@ $(LDFLAGS)

build/test-find-ci: tests/test_find_ci.cpp $(HEADERS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/test-find-ci-scalar: tests/test_find_ci.cpp $(HEADERS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -U__ARM_NEON $< -o $@

test-search: build/test-search-matching build/test-content build/test-find-ci build/test-find-ci-scalar
	./build/test-search-matching
	./build/test-content
	./build/test-find-ci
	./build/test-find-ci-scalar

-include $(OBJ:.o=.d)

.PHONY: clean lib test-search
