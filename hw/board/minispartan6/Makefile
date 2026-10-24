# Builds the FPGA image for the Scarab miniSpartan6+ with Xilinx ISE 14.7
# (ADR-0004), from the command line. Run on a machine with ISE installed:
#
#   source /opt/Xilinx/14.7/ISE_DS/settings64.sh
#   make            # -> build/top.bit (firmware built into the block RAM)
#   make prog       # load it into the FPGA over JTAG with xc3sprog
#   make flash      # write it to the SPI flash so it loads at power-on
#
# The firmware is built first by the main Makefile (RISC-V GCC); its
# block-RAM files are copied next to the XST run, where top.v's
# $readmemh calls find them.

ROOT    := ../../..
PART    := xc6slx25-ftg256-3
BUILD   := build
RTL     := $(addprefix $(ROOT)/hw/rtl/, soc.v bram.v uart_tx.v uart_rx.v sdram_ctrl.v paula.v \
             dac_dsm.v third_party/picorv32/picorv32.v) top.v
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
	  '-iob auto' > $@

$(BUILD)/top.ngc: $(RTL) $(BUILD)/top.prj $(BUILD)/top.xst firmware
	cp $(FW)/firmware0.hex $(FW)/firmware1.hex $(FW)/firmware2.hex $(FW)/firmware3.hex $(BUILD)/
	mkdir -p $(BUILD)/xst_tmp $(BUILD)/xst_hdp
	cd $(BUILD) && xst -ifn top.xst -ofn top.srp

$(BUILD)/top.ngd: $(BUILD)/top.ngc top.ucf
	cd $(BUILD) && ngdbuild -uc ../top.ucf -p $(PART) top.ngc top.ngd

# -pr b packs the SDRAM pin registers into the I/O blocks
$(BUILD)/top_map.ncd: $(BUILD)/top.ngd
	cd $(BUILD) && map -w -p $(PART) -pr b -o top_map.ncd top.ngd top.pcf

$(BUILD)/top.ncd: $(BUILD)/top_map.ncd
	cd $(BUILD) && par -w top_map.ncd top.ncd top.pcf
	cd $(BUILD) && trce -v 10 top.ncd top.pcf -o top.twr
	@grep -E "timing errors|Timing errors|Score" $(BUILD)/top.twr | head -5

$(BUILD)/top.bit: $(BUILD)/top.ncd
	cd $(BUILD) && bitgen -w -g StartUpClk:CClk -g CRC:Enable top.ncd top.bit

# xc3sprog drives the FT2232H's first channel as JTAG (http://xc3sprog.sourceforge.net)
prog: $(BUILD)/top.bit
	xc3sprog -c ftdi $(BUILD)/top.bit

# Needs xc3sprog's bscan_spi_xc6slx25.bit (from its sources) in this folder
flash: $(BUILD)/top.bit
	xc3sprog -c ftdi -I bscan_spi_xc6slx25.bit $(BUILD)/top.bit

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
