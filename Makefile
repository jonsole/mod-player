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

.PHONY: all test hw-test soc-test dsm-test firmware clean
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

test: $(UNIT_TESTS) all hw-test soc-test dsm-test
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

# ---- firmware (RISC-V soft CPU) ---------------------------------------------
RV_PREFIX ?= riscv64-unknown-elf-
RV_CFLAGS := -march=rv32im -mabi=ilp32 -Os -ffreestanding -nostdlib -Wall -Wextra -Werror \
             -ffunction-sections -fdata-sections -Ifirmware -Ifirmware/replayer
FW_SRC    := firmware/runtime/start.S firmware/runtime/libc.c firmware/main.c
FW_WORDS  := 8192

$(BUILD)/fw/firmware.elf: $(FW_SRC) firmware/runtime/link.ld $(wildcard firmware/*.h) | $(BUILD)
	mkdir -p $(BUILD)/fw
	$(RV_PREFIX)gcc $(RV_CFLAGS) -T firmware/runtime/link.ld -Wl,--gc-sections -o $@ $(FW_SRC) -lgcc
	$(RV_PREFIX)size $@

$(BUILD)/fw/firmware0.hex: $(BUILD)/fw/firmware.elf tools/bin2lanes.py
	$(RV_PREFIX)objcopy -O binary $< $(BUILD)/fw/firmware.bin
	python3 tools/bin2lanes.py $(BUILD)/fw/firmware.bin $(FW_WORDS) $(BUILD)/fw/firmware

firmware: $(BUILD)/fw/firmware0.hex

# ---- SoC simulation ---------------------------------------------------------
SOC_RTL := hw/rtl/soc.v hw/rtl/bram.v hw/rtl/uart_tx.v hw/rtl/third_party/picorv32/picorv32.v

$(BUILD)/soc_tb: $(SOC_RTL) hw/sim/soc_tb.cpp $(BUILD)/fw/firmware0.hex | $(BUILD)
	$(VERILATOR) $(VFLAGS) -Wno-lint -Wno-style --top-module soc \
		-GFIRMWARE='"$(CURDIR)/$(BUILD)/fw/firmware"' $(SOC_RTL) $(CURDIR)/hw/sim/soc_tb.cpp \
		-Mdir $(BUILD)/obj_soc -o soc_tb >$(BUILD)/soc_tb.log 2>&1 || (cat $(BUILD)/soc_tb.log; false)
	cp $(BUILD)/obj_soc/soc_tb $@

soc-test: $(BUILD)/soc_tb
	$(BUILD)/soc_tb "mod-player ready"

# ---- delta-sigma audio output ------------------------------------------------
$(BUILD)/dsm_tb: hw/rtl/dac_dsm.v hw/sim/dsm_tb.cpp | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module dac_dsm hw/rtl/dac_dsm.v $(CURDIR)/hw/sim/dsm_tb.cpp \
		-Mdir $(BUILD)/obj_dsm -o dsm_tb >$(BUILD)/dsm_tb.log 2>&1 || (cat $(BUILD)/dsm_tb.log; false)
	cp $(BUILD)/obj_dsm/dsm_tb $@

dsm-test: $(BUILD)/dsm_tb
	$(BUILD)/dsm_tb
