# PC-side build and tests. Needs only a host C compiler and Python 3.
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build
INC     := -Ifirmware/replayer -Ihost

REPLAYER_SRC := $(wildcard firmware/replayer/*.c)
HOST_TOOLS   := host/render.c host/replayer_log.c
HOST_SRC     := $(filter-out $(HOST_TOOLS),$(wildcard host/*.c))
ORACLE_SRC   := tests/oracle/pt2_log.c $(wildcard tests/oracle/pt2-clone/*.c) host/paula_log.c

UNIT_TESTS := $(patsubst tests/unit/%.c,$(BUILD)/%,$(wildcard tests/unit/test_*.c))

.PHONY: all test clean
all: $(BUILD)/render $(BUILD)/replayer_log $(BUILD)/pt2_log

$(BUILD):
	mkdir -p $@

$(BUILD)/render: host/render.c $(HOST_SRC) $(REPLAYER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/replayer_log: host/replayer_log.c $(HOST_SRC) $(REPLAYER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

# pt2-clone's replay code: test oracle only (ADR-0007). Built as-is, so its
# own warnings are not errors.
$(BUILD)/pt2_log: $(ORACLE_SRC) $(wildcard tests/oracle/stubs/*.h) | $(BUILD)
	$(CC) -std=gnu99 -O2 -Itests/oracle/stubs -Ifirmware/replayer -Ihost -o $@ $(ORACLE_SRC) -lm

$(BUILD)/test_%: tests/unit/test_%.c $(HOST_SRC) $(REPLAYER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

test: $(UNIT_TESTS) all
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; $$t; done
	@echo "== render command and pt2-clone oracle"; python3 -m unittest discover -s tests -p 'test_*.py'

clean:
	rm -rf $(BUILD)
