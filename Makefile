# PC-side build and tests. Needs only a host C compiler and Python 3.
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build
INC     := -Ifirmware/replayer -Ihost

REPLAYER_SRC := $(wildcard firmware/replayer/*.c)
HOST_SRC     := $(filter-out host/render.c,$(wildcard host/*.c))

UNIT_TESTS := $(patsubst tests/unit/%.c,$(BUILD)/%,$(wildcard tests/unit/test_*.c))

.PHONY: all test clean
all: $(BUILD)/render

$(BUILD):
	mkdir -p $@

$(BUILD)/render: host/render.c $(HOST_SRC) $(REPLAYER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_%: tests/unit/test_%.c $(HOST_SRC) $(REPLAYER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test: $(UNIT_TESTS) $(BUILD)/render
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; $$t; done
	@echo "== render command"; python3 -m unittest discover -s tests -p 'test_*.py'

clean:
	rm -rf $(BUILD)
