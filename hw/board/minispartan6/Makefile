# Builds the FPGA image for the Scarab miniSpartan6+ with Xilinx ISE 14.7
# (ADR-0004), from the command line. Run on a machine with ISE installed:
#
#   source /opt/Xilinx/14.7/ISE_DS/settings64.sh
#   make            # -> build/top.bit (the boot loader built into the block RAM)
#   make prog       # load it into the FPGA over JTAG with openFPGALoader
#   make flash      # write it and the firmware to the SPI flash so they load
#                   # at power-on (through the firmware over serial: PORT=/dev/ttyUSB1)
#
# On Windows, use build.bat instead (same steps, no make or RISC-V compiler).
#
# The boot loader and firmware are built first by the main Makefile (RISC-V
# GCC); the boot loader's block-RAM files are copied next to the XST run,
# where top.v's $readmemh calls find them. The firmware itself is loaded by
# the boot loader, from flash or serial (ADR-0011).

ROOT    := ../../..
PART    := xc6slx25-ftg256-3
BUILD   := build
RTL     := $(addprefix $(ROOT)/hw/rtl/, soc.v bram.v uart_tx.v uart_rx.v sdram_ctrl.v paula.v spi_master.v \
             dac_dsm.v third_party/picorv32/picorv32.v) top.v
UCF     := top.ucf
DEFINE  :=
# make HDMI=1: with HDMI output (ADR-0010), in build-hdmi/
ifeq ($(HDMI),1)
BUILD   := build-hdmi
RTL     += $(addprefix $(ROOT)/hw/rtl/, hdmi_out.v resampler.v hdmi_audio_fifo.v hdmi_tx.v tmds_encoder.v text_console.v) hdmi_phy.v
UCF     += hdmi.ucf
DEFINE  := '-define {HDMI}'
endif
FW      := $(ROOT)/build/fw

.PHONY: all firmware prog flash clean
all: $(BUILD)/top.bit

firmware:
	$(MAKE) -C $(ROOT) firmware

$(BUILD)/top.prj: Makefile | $(BUILD)
	rm -f $@
	for f in $(RTL); do echo "verilog work \"$(abspath $$f)\"" >> $@; done

$(BUILD)/top.xst: Makefile | $(BUILD)
	printf '%s\n' \
	  'set -tmpdir xst_tmp' \
	  'set -xsthdpdir xst_hdp' \
	  'run' \
	  '-ifn top.prj' \
	  '-ifmt mixed' \
	  '-top top' \
	  '-ofn top.ngc' \
	  '-ofmt NGC' \
	  '-p $(PART)' \
	  '-opt_mode Speed' \
	  '-opt_level 1' \
	  '-keep_hierarchy No' \
	  '-iob auto' \
	  '-uc ../top.xcf' $(DEFINE) > $@

$(BUILD)/top.ngc: $(RTL) top.xcf $(BUILD)/top.prj $(BUILD)/top.xst firmware
	cp $(FW)/boot0.hex $(FW)/boot1.hex $(FW)/boot2.hex $(FW)/boot3.hex $(BUILD)/
	cp $(ROOT)/hw/rtl/resampler_coefs.hex $(ROOT)/hw/rtl/console_font.hex $(BUILD)/
	mkdir -p $(BUILD)/xst_tmp $(BUILD)/xst_hdp
	cd $(BUILD) && xst -ifn top.xst -ofn top.srp

$(BUILD)/top.ngd: $(BUILD)/top.ngc $(UCF)
	cd $(BUILD) && ngdbuild $(addprefix -uc ../,$(UCF)) -p $(PART) top.ngc top.ngd

# -pr b packs the SDRAM pin registers into the I/O blocks
$(BUILD)/top_map.ncd: $(BUILD)/top.ngd
	cd $(BUILD) && map -w -p $(PART) -pr b -o top_map.ncd top.ngd top.pcf

$(BUILD)/top.ncd: $(BUILD)/top_map.ncd
	cd $(BUILD) && par -w top_map.ncd top.ncd top.pcf
	cd $(BUILD) && trce -v 10 top.ncd top.pcf -o top.twr
	@grep -E "timing errors|Timing errors|Score" $(BUILD)/top.twr | head -5

$(BUILD)/top.bit: $(BUILD)/top.ncd
	cd $(BUILD) && bitgen -w -g StartUpClk:CClk -g CRC:Enable top.ncd top.bit

# openFPGALoader knows this board: it drives the FT2232H's first channel as
# JTAG. (xc3sprog works too: xc3sprog -c ftdi build/top.bit)
prog: $(BUILD)/top.bit
	openFPGALoader -b minispartan6 $(BUILD)/top.bit

# openFPGALoader can't write this board's flash (docs/board.md, SPI flash), so
# load the image, send its boot loader the firmware (saving that to flash),
# and have the firmware write the image to flash over serial. PORT is the
# board's serial port.
PORT ?= /dev/ttyUSB1
flash: $(BUILD)/top.bit firmware
	openFPGALoader -b minispartan6 $(BUILD)/top.bit
	python3 $(ROOT)/tools/modplayer.py --port $(PORT) firmware --save $(FW)/firmware.bin
	python3 $(ROOT)/tools/modplayer.py --port $(PORT) flash-write $(BUILD)/top.bit

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
