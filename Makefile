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

.PHONY: all test hw-test clean
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
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE $(INC) -o $@ $^ -lm

test: $(UNIT_TESTS) all hw-test
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; $$t; done
	@echo "== render command and pt2-clone oracle"; python3 -m unittest discover -s tests -p 'test_*.py'

clean:
	rm -rf $(BUILD)

# ---- hardware simulation (Verilator) -----------------------------------------
VERILATOR ?= verilator
VFLAGS    := --cc --exe --build -j 4 -O3 -Wall -Wno-fatal
SIM_C     := host/paula_model.c $(REPLAYER_SRC)

$(BUILD)/paula_tb: hw/rtl/paula.v hw/sim/paula_tb.cpp $(SIM_C) $(wildcard firmware/replayer/*.h host/*.h) | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module paula -GFREE_RUN=1 hw/rtl/paula.v $(CURDIR)/hw/sim/paula_tb.cpp \
		$(addprefix $(CURDIR)/,$(SIM_C)) -CFLAGS "-O2 -I$(CURDIR)/host -I$(CURDIR)/firmware/replayer" \
		-Mdir $(BUILD)/obj_paula -o paula_tb >$(BUILD)/paula_tb.log 2>&1 || (cat $(BUILD)/paula_tb.log; false)
	cp $(BUILD)/obj_paula/paula_tb $@

# The same, paced at the board's real 50 MHz: Paula must keep up.
$(BUILD)/paula_tb_rt: hw/rtl/paula.v hw/sim/paula_tb.cpp $(SIM_C) $(wildcard firmware/replayer/*.h host/*.h) | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module paula -GFREE_RUN=0 hw/rtl/paula.v $(CURDIR)/hw/sim/paula_tb.cpp \
		$(addprefix $(CURDIR)/,$(SIM_C)) -CFLAGS "-O2 -I$(CURDIR)/host -I$(CURDIR)/firmware/replayer" \
		-Mdir $(BUILD)/obj_paula_rt -o paula_tb >$(BUILD)/paula_tb_rt.log 2>&1 || (cat $(BUILD)/paula_tb_rt.log; false)
	cp $(BUILD)/obj_paula_rt/paula_tb $@

# Verilog Paula vs the C model: every generated and real Song (first 5
# seconds each, options cycling through all 8 combinations) plus fuzzed
# register writes, run in parallel.
PAULA_SECONDS ?= 5
JOBS ?= $(shell nproc)
hw-test: $(BUILD)/paula_tb $(BUILD)/paula_tb_rt
	@rm -rf $(BUILD)/songs && python3 tools/modgen.py $(BUILD)/songs >/dev/null
	@i=0; for m in $(BUILD)/songs/*.mod tests/songs/*.mod; do \
		echo "$$m $(PAULA_SECONDS) $$((i % 8))"; i=$$((i + 1)); done > $(BUILD)/paula_runs
	@for s in 1 2 3 4 5 6 7 8; do echo "--fuzz $$s 300"; done >> $(BUILD)/paula_runs
	@(sed 's|^|$(BUILD)/paula_tb |' $(BUILD)/paula_runs; \
	  echo "$(BUILD)/paula_tb_rt tests/songs/gods.mod 2 7"; echo "$(BUILD)/paula_tb_rt --fuzz 9 300") \
		> $(BUILD)/paula_cmds
	@xargs -P $(JOBS) -L 1 sh -c '"$$0" "$$@"' < $(BUILD)/paula_cmds > $(BUILD)/paula_results; \
		status=$$?; cat $(BUILD)/paula_results; \
		echo "Verilog Paula: $$(grep -c ': ok' $(BUILD)/paula_results) of $$(wc -l < $(BUILD)/paula_cmds) runs match the C model"; \
		exit $$status
