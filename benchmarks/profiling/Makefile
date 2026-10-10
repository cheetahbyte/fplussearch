ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT ?= $(ROOT)/build/profiling
PCRE := $(shell pkg-config --variable=prefix libpcre2-8)
CXX ?= c++
FLAGS := -std=c++20 -O3 -g -mcpu=native -Wall -Wextra -Wpedantic -I$(ROOT)/src -isystem $(PCRE)/include
SOURCES := $(wildcard $(ROOT)/src/*.cpp)
OBJECTS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/%.o,$(SOURCES))
LIBOBJECTS := $(filter-out $(OUT)/main.o $(OUT)/tui.o,$(OBJECTS))
HEADERS := $(wildcard $(ROOT)/src/*.hpp)
LINK := -pthread -framework CoreServices $(PCRE)/lib/libpcre2-8.a

all: $(OUT)/replay $(OUT)/fplussearch

$(OUT):
	mkdir -p $@

$(OUT)/%.o: $(ROOT)/src/%.cpp $(HEADERS) | $(OUT)
	$(CXX) $(FLAGS) -c $< -o $@

$(OUT)/replay: $(ROOT)/benchmarks/profiling/replay.cpp $(LIBOBJECTS) $(HEADERS)
	$(CXX) $(FLAGS) $(ROOT)/benchmarks/profiling/replay.cpp $(LIBOBJECTS) -o $@ $(LINK)
	dsymutil $@

$(OUT)/fplussearch: $(OBJECTS)
	$(CXX) $(FLAGS) $^ -o $@ $(LINK)
	dsymutil $@

.PHONY: all
