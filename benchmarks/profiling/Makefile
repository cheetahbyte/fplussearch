ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT ?= $(ROOT)/build/profiling
PCRE := $(shell pkg-config --variable=prefix libpcre2-8)
CXX ?= c++
FLAGS := -std=c++20 -O3 -g -mcpu=native -Wall -Wextra -Wpedantic -I$(ROOT)/src -isystem $(PCRE)/include
SOURCES := $(shell find $(ROOT)/src -name '*.cpp' | sort)
OBJECTS := $(patsubst $(ROOT)/src/%.cpp,$(OUT)/%.o,$(SOURCES))
LIBOBJECTS := $(filter-out $(OUT)/main.o $(OUT)/cli/%,$(OBJECTS))
TOOLOBJECTS := $(OUT)/tools/replay.o $(OUT)/tools/measure.o
LINK := -pthread -framework CoreServices $(PCRE)/lib/libpcre2-8.a

all: $(OUT)/replay $(OUT)/fplussearch $(OUT)/measure

$(OUT):
	mkdir -p $@

$(OUT)/%.o: $(ROOT)/src/%.cpp | $(OUT)
	mkdir -p $(@D)
	$(CXX) $(FLAGS) -MMD -MP -c $< -o $@

$(OUT)/tools/%.o: $(ROOT)/benchmarks/profiling/%.cpp | $(OUT)
	mkdir -p $(@D)
	$(CXX) $(FLAGS) -MMD -MP -c $< -o $@

$(OUT)/replay: $(OUT)/tools/replay.o $(LIBOBJECTS)
	$(CXX) $(FLAGS) $^ -o $@ $(LINK)
	dsymutil $@

$(OUT)/measure: $(OUT)/tools/measure.o $(LIBOBJECTS)
	$(CXX) $(FLAGS) $^ -o $@ $(LINK)

$(OUT)/fplussearch: $(OBJECTS)
	$(CXX) $(FLAGS) $^ -o $@ $(LINK)
	dsymutil $@

-include $(OBJECTS:.o=.d) $(TOOLOBJECTS:.o=.d)

.PHONY: all
