CC = clang
AR ?= ar
SDKROOT ?= $(shell xcrun --sdk macosx --show-sdk-path)
MACOSX_DEPLOYMENT_TARGET ?= 13.0
ARCH ?= arm64
TARGET_FLAGS = -isysroot $(SDKROOT) -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) $(if $(ARCH),-arch $(ARCH))
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS += -std=c11 $(WARNINGS)
LDLIBS += -lcurl
BUILD ?= build/normal
SRC_ALL = $(wildcard src/util/*.c src/llm/*.c src/agent/*.c src/attractor/*.c)
OBJ_ALL = $(patsubst %.c,$(BUILD)/%.o,$(SRC_ALL))
LIB = $(BUILD)/libattractor.a
BIN = $(BUILD)/attractor
TEST = $(BUILD)/regressions

all: libattractor.a attractor
libattractor.a: $(LIB)
	cp $< $@
attractor: $(BIN)
	cp $< $@
$(LIB): $(OBJ_ALL)
	$(AR) rcs $@ $^
$(BIN): $(BUILD)/src/main.o $(LIB)
	$(CC) $(TARGET_FLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(TEST): $(BUILD)/test/regressions.o $(LIB)
	$(CC) $(TARGET_FLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(@D)
	$(CC) $(TARGET_FLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<
test: $(TEST)
	$(TEST)
sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-std=c11 $(WARNINGS) -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined,float-cast-overflow' LDFLAGS='-fsanitize=address,undefined,float-cast-overflow' test
analyze:
	@mkdir -p build/analyze
	@set -e; for source in $(SRC_ALL) src/main.c; do $(CC) $(TARGET_FLAGS) $(CPPFLAGS) $(CFLAGS) -Werror --analyze -Xanalyzer -analyzer-werror -Xanalyzer -analyzer-output=plist -o build/analyze/$$(basename $$source).plist $$source; done
ci:
	$(MAKE) BUILD=build/ci CFLAGS='-std=c11 $(WARNINGS) -Werror -O2 -g' build/ci/attractor test
	$(MAKE) analyze
debug:
	$(MAKE) BUILD=build/debug CFLAGS='-std=c11 $(WARNINGS) -Werror -O0 -g' build/debug/attractor build/debug/regressions
leaks: debug
	leaks --atExit -- build/debug/regressions --leaks > build/debug/leaks.log 2>&1
	cat build/debug/leaks.log
	grep -q '^All selected regressions passed' build/debug/leaks.log
	grep -q '0 leaks for 0 total leaked bytes' build/debug/leaks.log
macos-arm64:
	$(MAKE) BUILD=build/arm64 ARCH=arm64 CFLAGS='-std=c11 $(WARNINGS) -Werror -O2 -g' build/arm64/attractor build/arm64/regressions
clean:
	rm -rf build
	rm -f libattractor.a attractor

-include $(OBJ_ALL:.o=.d) $(BUILD)/src/main.d $(BUILD)/test/regressions.d
.PHONY: all test sanitize analyze ci debug leaks macos-arm64 clean
