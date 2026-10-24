# A boot loader runs firmware from SPI flash; flash also holds a saved Song

The FPGA image contains only a small boot loader. At power-on it copies the firmware from the board's SPI flash into RAM and runs it, unless the PC script asks to send new firmware over USB serial instead. This makes firmware changes a seconds-long upload instead of a full ISE rebuild. The same 8 MB flash holds, in order, the FPGA image, the firmware, and one saved Song that plays at power-up without a PC.

## Consequences

- Firmware and saved Song live at fixed flash offsets that the boot loader, firmware and PC script must agree on.
- Replacing the FPGA image never erases the saved Song or firmware as long as the image stays within its reserved region.
