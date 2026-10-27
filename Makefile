# Build:   make            -> optimized engine at build/release/engine
#          make debug      -> sanitized debug engine at build/debug/engine
#          make run        -> build release and start it
#          make test       -> quick self-tests on the sanitized debug build
#          make perft      -> full perft gate on the release build
#          make bench      -> bench signature (node count) for commit messages
#          make clean

EXE      := engine
CXX      := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -MMD -MP

RELEASE_FLAGS := -O3 -march=native -DNDEBUG -flto
DEBUG_FLAGS   := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

SRCS := $(wildcard src/*.cpp)

BUILD ?= release
ifeq ($(BUILD),debug)
  CXXFLAGS += $(DEBUG_FLAGS)
  LDFLAGS  += -fsanitize=address,undefined
else
  CXXFLAGS += $(RELEASE_FLAGS)
  LDFLAGS  += -flto
endif

OUT  := build/$(BUILD)
OBJS := $(SRCS:src/%.cpp=$(OUT)/%.o)
BIN  := $(OUT)/$(EXE)

.PHONY: all release debug run test perft bench clean

all: $(BIN)

release:
	@$(MAKE) --no-print-directory BUILD=release

debug:
	@$(MAKE) --no-print-directory BUILD=debug

run: release
	./build/release/$(EXE)

test: debug
	./build/debug/$(EXE) test

perft: release
	./build/release/$(EXE) test full

bench: release
	@./build/release/$(EXE) bench | grep -E "Nodes searched|NPS"

$(BIN): $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@

$(OUT)/%.o: src/%.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf build

-include $(OBJS:.o=.d)
