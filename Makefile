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

.PHONY: all test hw-test soc-test boot-test console-test font-check dsm-test fullchip-test board-test synth-check firmware board-firmware firmware-check clean \
        coefs-check resampler-test hdmi-test hdmi-chain-test hdmi-board-check flash-model-test
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

test: $(UNIT_TESTS) all hw-test flash-model-test soc-test boot-test console-test font-check dsm-test fullchip-test board-test firmware-check \
      resampler-test hdmi-test hdmi-chain-test
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; $$t; done
	@echo "== render command, pt2-clone oracle, PC script"; python3 -m unittest tests.test_render tests.test_oracle tests.test_modplayer

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
FW_SRC    := firmware/runtime/start.S firmware/runtime/libc.c $(wildcard firmware/*.c) $(REPLAYER_SRC)
FW_WORDS  := 16384

$(BUILD)/fw/firmware.elf: $(FW_SRC) firmware/runtime/link.ld $(wildcard firmware/*.h firmware/replayer/*.h) | $(BUILD)
	mkdir -p $(BUILD)/fw
	$(RV_PREFIX)gcc $(RV_CFLAGS) -T firmware/runtime/link.ld -Wl,--gc-sections,--no-warn-rwx-segments -o $@ $(FW_SRC) -lgcc
	$(RV_PREFIX)size $@

$(BUILD)/fw/firmware0.hex: $(BUILD)/fw/firmware.elf tools/bin2lanes.py
	$(RV_PREFIX)objcopy -O binary $< $(BUILD)/fw/firmware.bin
	python3 tools/bin2lanes.py $(BUILD)/fw/firmware.bin $(FW_WORDS) $(BUILD)/fw/firmware

# The boot loader (ADR-0011): at 0xF000, the top 4 KB of the same RAM. Its
# RAM image (boot0..3.hex) is what the FPGA image holds; it loads
# firmware.bin from flash or serial. Firmware lives at address 0, so writes
# through a "null" pointer are meant: -fno-delete-null-pointer-checks.
BOOT_SRC  := firmware/boot/start.S firmware/boot/boot.c firmware/runtime/libc.c \
             firmware/serial.c firmware/crc32.c firmware/flash.c

$(BUILD)/fw/boot.elf: $(BOOT_SRC) firmware/boot/link.ld $(wildcard firmware/*.h) | $(BUILD)
	mkdir -p $(BUILD)/fw
	$(RV_PREFIX)gcc $(RV_CFLAGS) -fno-delete-null-pointer-checks -T firmware/boot/link.ld \
		-Wl,--gc-sections,--no-warn-rwx-segments -o $@ $(BOOT_SRC) -lgcc
	$(RV_PREFIX)size $@

$(BUILD)/fw/boot0.hex: $(BUILD)/fw/boot.elf tools/bin2lanes.py
	$(RV_PREFIX)objcopy -O binary $< $(BUILD)/fw/boot.bin
	python3 tools/bin2lanes.py $(BUILD)/fw/boot.bin $(FW_WORDS) $(BUILD)/fw/boot 0xF000

firmware: $(BUILD)/fw/firmware0.hex $(BUILD)/fw/boot0.hex

# A ready-built copy is kept with the board files, so the FPGA can be built
# (e.g. with ISE on Windows) without a RISC-V compiler: the boot loader's RAM
# image for the FPGA build, and firmware.bin for modplayer.py firmware.
# `make board-firmware` refreshes it; `make test` checks it is up to date.
BOARD_FW := hw/board/minispartan6/firmware
BOARD_FW_FILES := boot0.hex boot1.hex boot2.hex boot3.hex firmware.bin
board-firmware: firmware
	cp $(addprefix $(BUILD)/fw/,$(BOARD_FW_FILES)) $(BOARD_FW)/

firmware-check: firmware
	@for f in $(BOARD_FW_FILES); do cmp -s $(BUILD)/fw/$$f $(BOARD_FW)/$$f || \
	  { echo "$(BOARD_FW) is out of date: run 'make board-firmware' and commit it"; exit 1; }; done
	@echo "== ready-built board firmware is up to date"

# ---- SoC simulation ---------------------------------------------------------
SOC_RTL := hw/rtl/soc.v hw/rtl/bram.v hw/rtl/uart_tx.v hw/rtl/uart_rx.v hw/rtl/sdram_ctrl.v hw/rtl/paula.v hw/rtl/spi_master.v \
           hw/rtl/third_party/picorv32/picorv32.v

SOC_TB_DEPS := $(SOC_RTL) hw/sim/soc_tb.cpp hw/sim/sdram_model.h hw/sim/spi_flash_model.h $(SIM_C) $(BUILD)/fw/firmware0.hex
SOC_TB_ARGS = $(VFLAGS) -Wno-lint -Wno-style --top-module soc \
	$(foreach n,0 1 2 3,-GFW$(n)='"$(CURDIR)/$(BUILD)/fw/firmware$(n).hex"') $(SOC_RTL) $(CURDIR)/hw/sim/soc_tb.cpp \
	$(addprefix $(CURDIR)/,$(SIM_C)) \
	-CFLAGS "-O2 -I$(CURDIR)/hw/sim -I$(CURDIR)/host -I$(CURDIR)/firmware/replayer"

$(BUILD)/soc_tb: $(SOC_TB_DEPS) | $(BUILD)
	$(VERILATOR) $(SOC_TB_ARGS) -Mdir $(BUILD)/obj_soc -o soc_tb \
		>$(BUILD)/soc_tb.log 2>&1 || (cat $(BUILD)/soc_tb.log; false)
	cp $(BUILD)/obj_soc/soc_tb $@

# Paula unpaced, for comparing long stretches of audio quickly
$(BUILD)/soc_tb_fast: $(SOC_TB_DEPS) | $(BUILD)
	$(VERILATOR) $(SOC_TB_ARGS) -GPAULA_FREE_RUN=1 -Mdir $(BUILD)/obj_soc_fast -o soc_tb \
		>$(BUILD)/soc_tb_fast.log 2>&1 || (cat $(BUILD)/soc_tb_fast.log; false)
	cp $(BUILD)/obj_soc_fast/soc_tb $@

soc-test: $(BUILD)/soc_tb
	$(BUILD)/soc_tb "ready"
	python3 -m unittest tests.test_soc

# The SoC as the FPGA image has it: only the boot loader in RAM, started at
# its power-on entry (issue #20)
$(BUILD)/soc_boot_tb: $(SOC_TB_DEPS) $(BUILD)/fw/boot0.hex | $(BUILD)
	$(VERILATOR) $(VFLAGS) -Wno-lint -Wno-style --top-module soc \
		$(foreach n,0 1 2 3,-GFW$(n)='"$(CURDIR)/$(BUILD)/fw/boot$(n).hex"') -GRESET_ADDR=61440 \
		$(SOC_RTL) $(CURDIR)/hw/sim/soc_tb.cpp $(addprefix $(CURDIR)/,$(SIM_C)) \
		-CFLAGS "-O2 -I$(CURDIR)/hw/sim -I$(CURDIR)/host -I$(CURDIR)/firmware/replayer" \
		-Mdir $(BUILD)/obj_soc_boot -o soc_tb >$(BUILD)/soc_boot_tb.log 2>&1 || (cat $(BUILD)/soc_boot_tb.log; false)
	cp $(BUILD)/obj_soc_boot/soc_tb $@

boot-test: $(BUILD)/soc_boot_tb $(BUILD)/fw/firmware0.hex
	python3 -m unittest tests.test_boot

# ---- delta-sigma audio output ------------------------------------------------
$(BUILD)/dsm_tb: hw/rtl/dac_dsm.v hw/sim/dsm_tb.cpp | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module dac_dsm hw/rtl/dac_dsm.v $(CURDIR)/hw/sim/dsm_tb.cpp \
		-Mdir $(BUILD)/obj_dsm -o dsm_tb >$(BUILD)/dsm_tb.log 2>&1 || (cat $(BUILD)/dsm_tb.log; false)
	cp $(BUILD)/obj_dsm/dsm_tb $@

dsm-test: $(BUILD)/dsm_tb
	$(BUILD)/dsm_tb

# The whole chip plays each Song (put in SDRAM through the back door, then
# started with the M command); Paula's audio must equal the PC reference.
FULLCHIP_SECONDS ?= 5
fullchip-test: $(BUILD)/soc_tb_fast $(BUILD)/soc_tb
	@rm -rf $(BUILD)/songs && python3 tools/modgen.py $(BUILD)/songs >/dev/null
	@i=0; for m in $(BUILD)/songs/*.mod tests/songs/*.mod; do \
		python3 -c "import sys; sys.path.insert(0, 'tools'); import modplayer, os; \
			open('$$m.cmd', 'wb').write(modplayer.encode_play_from_memory(os.path.getsize('$$m')))"; \
		echo "$(BUILD)/soc_tb_fast --after ready --switches $$((i % 4)) --preload $$m --send $$m.cmd \
			--compare $$m $(FULLCHIP_SECONDS) --max 2000000000"; i=$$((i + 1)); done > $(BUILD)/fullchip_cmds
	@m=$(BUILD)/songs/volume-set.mod; echo "$(BUILD)/soc_tb_fast --after ready --switches 4 --preload $$m \
		--send $$m.cmd --compare $$m 9 --max 2000000000" >> $(BUILD)/fullchip_cmds  # play once: ends at 7.7 s
	@m=$(BUILD)/songs/arpeggio.mod; echo "$(BUILD)/soc_tb --after ready --switches 3 --preload $$m \
		--send $$m.cmd --compare $$m 1 --max 2000000000" >> $(BUILD)/fullchip_cmds  # real time, at 50 MHz
	@xargs -P $(JOBS) -I{} sh -c '{} | sed "s|^|$$(echo {} | grep -o "[^ ]*\.mod" | head -1): |"' \
		< $(BUILD)/fullchip_cmds > $(BUILD)/fullchip_results; status=$$?; \
		grep -E "frames match|FAIL" $(BUILD)/fullchip_results; \
		echo "Full chip: $$(grep -c '^.*ok: ' $(BUILD)/fullchip_results) of $$(wc -l < $(BUILD)/fullchip_cmds) runs match the PC reference"; \
		exit $$status

# ---- board top level ---------------------------------------------------------
BOARD_RTL := hw/board/minispartan6/top.v hw/board/minispartan6/sim_primitives.v hw/rtl/dac_dsm.v $(SOC_RTL)

$(BUILD)/board_tb: $(BOARD_RTL) hw/sim/board_wrap.v hw/sim/board_tb.cpp hw/sim/sdram_model.h $(BUILD)/fw/firmware0.hex | $(BUILD)
	$(VERILATOR) $(VFLAGS) -Wno-lint -Wno-style --top-module board_wrap \
		+define+FIRMWARE='"$(CURDIR)/$(BUILD)/fw/firmware"' hw/sim/board_wrap.v $(BOARD_RTL) \
		$(CURDIR)/hw/sim/board_tb.cpp -CFLAGS "-O2 -I$(CURDIR)/hw/sim" \
		-Mdir $(BUILD)/obj_board -o board_tb >$(BUILD)/board_tb.log 2>&1 || (cat $(BUILD)/board_tb.log; false)
	cp $(BUILD)/obj_board/board_tb $@

# Boots the board top level, plays a left-only Song, checks the audio pins
board-test: $(BUILD)/board_tb
	@python3 -c "import sys; sys.path.insert(0, 'tools'); import modgen; s = modgen.Song('left only'); \
		s.sample(1, modgen.square_cycle(32) * 4, volume=64, loop=(0, 128)); s.note(0, 0, 0, period=428, sample=1); \
		open('$(BUILD)/left-only.mod', 'wb').write(s.build())"
	$(BUILD)/board_tb $(BUILD)/left-only.mod

# Synthesis check with Yosys for Spartan-6 (a stand-in for XST: catches
# constructs that don't synthesise and reports resource use)
synth-check: $(BUILD)/fw/boot0.hex
	@echo "read_verilog $(addprefix $(CURDIR)/,$(filter-out %sim_primitives.v,$(BOARD_RTL)))" > $(BUILD)/synth.ys
	@echo "synth_xilinx -family xc6s -top top" >> $(BUILD)/synth.ys
	@echo "stat" >> $(BUILD)/synth.ys
	@# run next to the firmware files, as the ISE build does
	cd $(BUILD)/fw && yosys -q -l ../yosys.log -s ../synth.ys >/dev/null
	@echo "Spartan-6 resources (Yosys estimate; the XC6SLX25 has 15032 LUTs, 30064 flip-flops,"
	@echo "38 DSP48A1 and 52 RAMB16):"
	@python3 tools/yosys_stats.py $(BUILD)/yosys.log

# ---- HDMI Resampler (ADR-0010) ------------------------------------------------
# The coefficient table is generated once and committed; this checks it is
# what tools/resampler_coefs.py makes.
coefs-check: | $(BUILD)
	@python3 tools/resampler_coefs.py $(BUILD)/resampler_coefs.h $(BUILD)/resampler_coefs.hex >/dev/null
	@cmp -s $(BUILD)/resampler_coefs.h host/resampler_coefs.h && cmp -s $(BUILD)/resampler_coefs.hex hw/rtl/resampler_coefs.hex || \
	  { echo "resampler coefficients are out of date: run tools/resampler_coefs.py host/resampler_coefs.h hw/rtl/resampler_coefs.hex"; exit 1; }
	@echo "== resampler coefficients are up to date"

$(BUILD)/resampler_tb: hw/rtl/resampler.v hw/rtl/resampler_coefs.hex hw/sim/resampler_tb.cpp $(SIM_C) host/resampler.c \
		$(wildcard firmware/replayer/*.h host/*.h) | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module resampler -GCOEFS='"$(CURDIR)/hw/rtl/resampler_coefs.hex"' hw/rtl/resampler.v \
		$(CURDIR)/hw/sim/resampler_tb.cpp $(addprefix $(CURDIR)/,$(SIM_C) host/resampler.c) \
		-CFLAGS "-O2 -I$(CURDIR)/host -I$(CURDIR)/firmware/replayer" \
		-Mdir $(BUILD)/obj_resampler -o resampler_tb >$(BUILD)/resampler_tb.log 2>&1 || (cat $(BUILD)/resampler_tb.log; false)
	cp $(BUILD)/obj_resampler/resampler_tb $@

# Verilog Resampler vs the C model: noise, full-scale squares, and every
# generated and real Song (options cycling), run in parallel.
RESAMPLER_SECONDS ?= 2
resampler-test: $(BUILD)/resampler_tb coefs-check
	@rm -rf $(BUILD)/songs && python3 tools/modgen.py $(BUILD)/songs >/dev/null
	@(for s in 1 2 3 4; do echo "--noise $$s 1"; done; echo "--square 1"; \
	  i=0; for m in $(BUILD)/songs/*.mod tests/songs/*.mod; do \
		echo "$$m $(RESAMPLER_SECONDS) $$((i % 8))"; i=$$((i + 1)); done) \
		| sed 's|^|$(BUILD)/resampler_tb |' > $(BUILD)/resampler_cmds
	@xargs -P $(JOBS) -L 1 sh -c '"$$0" "$$@"' < $(BUILD)/resampler_cmds > $(BUILD)/resampler_results; \
		status=$$?; cat $(BUILD)/resampler_results; \
		echo "Verilog Resampler: $$(grep -c ': ok' $(BUILD)/resampler_results) of $$(wc -l < $(BUILD)/resampler_cmds) runs match the C model"; \
		exit $$status

# ---- HDMI transmitter (ADR-0010) ------------------------------------------------
HDMI_RTL := hw/rtl/hdmi_tx.v hw/rtl/tmds_encoder.v

$(BUILD)/hdmi_tb: $(HDMI_RTL) hw/sim/hdmi_tb.cpp hw/sim/hdmi_sink.h | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module hdmi_tx $(HDMI_RTL) $(CURDIR)/hw/sim/hdmi_tb.cpp \
		-CFLAGS "-O2 -I$(CURDIR)/hw/sim" \
		-Mdir $(BUILD)/obj_hdmi -o hdmi_tb >$(BUILD)/hdmi_tb.log 2>&1 || (cat $(BUILD)/hdmi_tb.log; false)
	cp $(BUILD)/obj_hdmi/hdmi_tb $@

# the same with the picture 5 clocks behind the timing, as the text console has it
$(BUILD)/hdmi_tb_d5: $(HDMI_RTL) hw/sim/hdmi_tb.cpp hw/sim/hdmi_sink.h | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module hdmi_tx -GRGB_DELAY=5 $(HDMI_RTL) $(CURDIR)/hw/sim/hdmi_tb.cpp \
		-CFLAGS "-O2 -I$(CURDIR)/hw/sim" \
		-Mdir $(BUILD)/obj_hdmi_d5 -o hdmi_tb >$(BUILD)/hdmi_tb_d5.log 2>&1 || (cat $(BUILD)/hdmi_tb_d5.log; false)
	cp $(BUILD)/obj_hdmi_d5/hdmi_tb $@

$(BUILD)/tmds_tb: hw/rtl/tmds_encoder.v hw/sim/tmds_tb.cpp | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module tmds_encoder hw/rtl/tmds_encoder.v $(CURDIR)/hw/sim/tmds_tb.cpp \
		-Mdir $(BUILD)/obj_tmds -o tmds_tb >$(BUILD)/tmds_tb.log 2>&1 || (cat $(BUILD)/tmds_tb.log; false)
	cp $(BUILD)/obj_tmds/tmds_tb $@

# The TMDS encoder on random bytes; then the transmitter's output, decoded
# as a sink would, checked against the HDMI rules
$(BUILD)/serialiser_tb: hw/board/minispartan6/hdmi_phy.v hw/sim/serialiser_tb.cpp | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module tmds_serialiser hw/board/minispartan6/hdmi_phy.v \
		hw/board/minispartan6/sim_hdmi_primitives.v hw/board/minispartan6/sim_primitives.v $(CURDIR)/hw/sim/serialiser_tb.cpp \
		-Mdir $(BUILD)/obj_serialiser -o serialiser_tb >$(BUILD)/serialiser_tb.log 2>&1 || (cat $(BUILD)/serialiser_tb.log; false)
	cp $(BUILD)/obj_serialiser/serialiser_tb $@

hdmi-test: $(BUILD)/tmds_tb $(BUILD)/hdmi_tb $(BUILD)/hdmi_tb_d5 $(BUILD)/serialiser_tb
	$(BUILD)/tmds_tb 1 2000000
	$(BUILD)/hdmi_tb 4
	$(BUILD)/hdmi_tb_d5 3 5
	$(BUILD)/serialiser_tb

# The whole HDMI audio chain on two clocks: Songs' Paula frames in, decoded
# HDMI audio out, equal to the Resampler C model's output
HDMI_OUT_RTL := hw/rtl/hdmi_out.v hw/rtl/resampler.v hw/rtl/hdmi_audio_fifo.v hw/rtl/text_console.v $(HDMI_RTL)
$(BUILD)/hdmi_chain_tb: $(HDMI_OUT_RTL) hw/rtl/resampler_coefs.hex hw/rtl/console_font.hex hw/sim/hdmi_chain_tb.cpp hw/sim/hdmi_sink.h \
		$(SIM_C) host/resampler.c $(wildcard firmware/replayer/*.h host/*.h) | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module hdmi_out -GCOEFS='"$(CURDIR)/hw/rtl/resampler_coefs.hex"' \
		-GFONT='"$(CURDIR)/hw/rtl/console_font.hex"' $(HDMI_OUT_RTL) \
		$(CURDIR)/hw/sim/hdmi_chain_tb.cpp $(addprefix $(CURDIR)/,$(SIM_C) host/resampler.c) \
		-CFLAGS "-O2 -I$(CURDIR)/hw/sim -I$(CURDIR)/host -I$(CURDIR)/firmware/replayer" \
		-Mdir $(BUILD)/obj_hdmi_chain -o hdmi_chain_tb >$(BUILD)/hdmi_chain_tb.log 2>&1 || (cat $(BUILD)/hdmi_chain_tb.log; false)
	cp $(BUILD)/obj_hdmi_chain/hdmi_chain_tb $@

HDMI_CHAIN_SECONDS ?= 2
# The text console end to end (issue #21): CPU and firmware, HDMI out,
# every frame decoded and checked against the console model
$(BUILD)/console_tb: $(SOC_RTL) $(HDMI_OUT_RTL) hw/rtl/console_font.hex hw/rtl/resampler_coefs.hex \
		hw/sim/console_wrap.v hw/sim/console_tb.cpp hw/sim/hdmi_sink.h hw/sim/sdram_model.h \
		$(BUILD)/fw/firmware0.hex | $(BUILD)
	$(VERILATOR) $(VFLAGS) -Wno-lint -Wno-style --top-module console_wrap \
		$(foreach n,0 1 2 3,-GFW$(n)='"$(CURDIR)/$(BUILD)/fw/firmware$(n).hex"') \
		-GCOEFS='"$(CURDIR)/hw/rtl/resampler_coefs.hex"' -GFONT='"$(CURDIR)/hw/rtl/console_font.hex"' \
		hw/sim/console_wrap.v $(SOC_RTL) $(HDMI_OUT_RTL) $(CURDIR)/hw/sim/console_tb.cpp \
		-CFLAGS "-O2 -I$(CURDIR)/hw/sim" \
		-Mdir $(BUILD)/obj_console -o console_tb >$(BUILD)/console_tb.log 2>&1 || (cat $(BUILD)/console_tb.log; false)
	cp $(BUILD)/obj_console/console_tb $@

# its bus side alone, with writes as fast as the bus allows
$(BUILD)/text_console_tb: hw/rtl/text_console.v hw/rtl/console_font.hex hw/sim/text_console_tb.cpp | $(BUILD)
	$(VERILATOR) $(VFLAGS) --top-module text_console -GFONT='"$(CURDIR)/hw/rtl/console_font.hex"' hw/rtl/text_console.v \
		$(CURDIR)/hw/sim/text_console_tb.cpp -Mdir $(BUILD)/obj_text_console -o text_console_tb \
		>$(BUILD)/text_console_tb.log 2>&1 || (cat $(BUILD)/text_console_tb.log; false)
	cp $(BUILD)/obj_text_console/text_console_tb $@

console-test: $(BUILD)/console_tb $(BUILD)/text_console_tb
	$(BUILD)/text_console_tb 400000
	python3 -m unittest tests.test_console

# The font is made from Spleen (hw/rtl/third_party/spleen) by
# tools/console_font.py and committed; this checks it is what the tool makes.
font-check: | $(BUILD)
	@python3 tools/console_font.py $(BUILD)/console_font.hex >/dev/null
	@cmp -s $(BUILD)/console_font.hex hw/rtl/console_font.hex || \
	  { echo "hw/rtl/console_font.hex is out of date: run tools/console_font.py hw/rtl/console_font.hex"; exit 1; }
	@echo "== console font is up to date"

hdmi-chain-test: $(BUILD)/hdmi_chain_tb
	@rm -rf $(BUILD)/songs && python3 tools/modgen.py $(BUILD)/songs >/dev/null
	@(echo "tests/songs/gods.mod $(HDMI_CHAIN_SECONDS) 7"; echo "$(BUILD)/songs/vibrato.mod $(HDMI_CHAIN_SECONDS) 0"; \
	  echo "$(BUILD)/songs/retrigger.mod $(HDMI_CHAIN_SECONDS) 2") | sed 's|^|$(BUILD)/hdmi_chain_tb |' > $(BUILD)/hdmi_chain_cmds
	@xargs -P $(JOBS) -L 1 sh -c '"$$0" "$$@"' < $(BUILD)/hdmi_chain_cmds > $(BUILD)/hdmi_chain_results; \
		status=$$?; cat $(BUILD)/hdmi_chain_results; exit $$status

# The board top level with HDMI (`define HDMI): Verilator lint, then a Yosys
# synthesis for resource use (the ISE build is build.bat hdmi)
HDMI_BOARD_RTL := $(BOARD_RTL) $(HDMI_OUT_RTL) hw/board/minispartan6/hdmi_phy.v
hdmi-board-check: $(BUILD)/fw/boot0.hex
	$(VERILATOR) --lint-only -Wno-fatal -Wno-lint -Wno-style +define+HDMI --top-module top \
		$(HDMI_BOARD_RTL) hw/board/minispartan6/sim_hdmi_primitives.v 2>&1 | grep -E '%Error' && exit 1 || true
	@echo "read_verilog -DHDMI $(addprefix $(CURDIR)/,$(filter-out %sim_primitives.v,$(HDMI_BOARD_RTL)))" > $(BUILD)/synth_hdmi.ys
	@echo "synth_xilinx -family xc6s -top top" >> $(BUILD)/synth_hdmi.ys
	@echo "stat" >> $(BUILD)/synth_hdmi.ys
	cp hw/rtl/resampler_coefs.hex hw/rtl/console_font.hex $(BUILD)/fw/
	cd $(BUILD)/fw && yosys -q -l ../yosys_hdmi.log -s ../synth_hdmi.ys >/dev/null
	@echo "Spartan-6 resources with HDMI (Yosys estimate; the XC6SLX25 has 15032 LUTs, 30064 flip-flops,"
	@echo "38 DSP48A1 and 52 RAMB16):"
	@python3 tools/yosys_stats.py $(BUILD)/yosys_hdmi.log

# ---- SPI flash model (issue #17) ---------------------------------------------------
# The model the SoC testbench uses: answers like the chip, flags mistakes
$(BUILD)/spi_flash_model_test: hw/sim/spi_flash_model_test.cpp hw/sim/spi_flash_model.h | $(BUILD)
	$(CXX) -std=c++17 -O1 -Wall -Wextra -Werror -Ihw/sim -o $@ $<

flash-model-test: $(BUILD)/spi_flash_model_test
	$(BUILD)/spi_flash_model_test
