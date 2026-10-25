@echo off
rem Builds the FPGA image for the Scarab miniSpartan6+ with Xilinx ISE 14.7 on
rem Windows: the same steps as the Makefile next to it. From a Command Prompt:
rem
rem   build.bat          build build\top.bit
rem   build.bat prog     build, then load it into the FPGA (lost at power-off)
rem   build.bat flash    explains how to write the SPI flash (loads at power-on): the
rem                      firmware does it over serial (docs\board.md, SPI flash)
rem   build.bat hdmi     the same with HDMI output (ADR-0010), in build-hdmi\top.bit;
rem                      also build.bat hdmi prog
rem
rem ISE is looked for in D:\Xilinx\14.7\ISE_DS, then C:\Xilinx\14.7\ISE_DS; set
rem ISE_DIR to use another folder. prog and flash need openFPGALoader on the
rem PATH (see docs\board.md). The firmware comes from the firmware folder here,
rem which the main Makefile keeps up to date, so no RISC-V compiler is needed.

setlocal
set PART=xc6slx25-ftg256-3
set HERE=%~dp0
set ACTION=%1
set HDMI=
set OUT=build
if /i "%1"=="hdmi" (
  set HDMI=1
  set OUT=build-hdmi
  set ACTION=%2
)

if not defined ISE_DIR if exist "D:\Xilinx\14.7\ISE_DS\settings64.bat" set ISE_DIR=D:\Xilinx\14.7\ISE_DS
if not defined ISE_DIR if exist "C:\Xilinx\14.7\ISE_DS\settings64.bat" set ISE_DIR=C:\Xilinx\14.7\ISE_DS
if not defined ISE_DIR (
  echo Can't find ISE 14.7. Set ISE_DIR to the folder holding settings64.bat, e.g.
  echo   set ISE_DIR=D:\Xilinx\14.7\ISE_DS
  exit /b 1
)
call "%ISE_DIR%\settings64.bat" >nul

cd /d "%HERE%"
if not exist %OUT% mkdir %OUT%
cd %OUT%
if not exist xst_tmp mkdir xst_tmp
if not exist xst_hdp mkdir xst_hdp
copy /y ..\firmware\firmware?.hex . >nul || goto fail
if defined HDMI copy /y ..\..\..\rtl\resampler_coefs.hex . >nul || goto fail

set RTL=..\..\..\..\hw\rtl
> top.prj echo verilog work "%RTL%\soc.v"
>> top.prj echo verilog work "%RTL%\bram.v"
>> top.prj echo verilog work "%RTL%\uart_tx.v"
>> top.prj echo verilog work "%RTL%\uart_rx.v"
>> top.prj echo verilog work "%RTL%\sdram_ctrl.v"
>> top.prj echo verilog work "%RTL%\paula.v"
>> top.prj echo verilog work "%RTL%\spi_master.v"
>> top.prj echo verilog work "%RTL%\dac_dsm.v"
>> top.prj echo verilog work "%RTL%\third_party\picorv32\picorv32.v"
>> top.prj echo verilog work "..\top.v"
if defined HDMI (
  >> top.prj echo verilog work "%RTL%\hdmi_out.v"
  >> top.prj echo verilog work "%RTL%\resampler.v"
  >> top.prj echo verilog work "%RTL%\hdmi_audio_fifo.v"
  >> top.prj echo verilog work "%RTL%\hdmi_tx.v"
  >> top.prj echo verilog work "%RTL%\tmds_encoder.v"
  >> top.prj echo verilog work "..\hdmi_phy.v"
)

> top.xst echo set -tmpdir xst_tmp
>> top.xst echo set -xsthdpdir xst_hdp
>> top.xst echo run
>> top.xst echo -ifn top.prj
>> top.xst echo -ifmt mixed
>> top.xst echo -top top
>> top.xst echo -ofn top.ngc
>> top.xst echo -ofmt NGC
>> top.xst echo -p %PART%
>> top.xst echo -opt_mode Speed
>> top.xst echo -opt_level 1
>> top.xst echo -keep_hierarchy No
>> top.xst echo -iob auto
>> top.xst echo -uc ..\top.xcf
if defined HDMI >> top.xst echo -define {HDMI}
set UCF=-uc ..\top.ucf
if defined HDMI set UCF=-uc ..\top.ucf -uc ..\hdmi.ucf

echo === xst (synthesis)
xst -ifn top.xst -ofn top.srp || goto fail
echo === ngdbuild
ngdbuild %UCF% -p %PART% top.ngc top.ngd || goto fail
echo === map
rem -pr b packs the SDRAM pin registers into the I/O blocks.
rem On Windows 11, ISE 14.7's map sometimes dies during global placement with
rem no message; the same run then succeeds, so it is tried up to three times.
set MAP_TRIES=0
:map_again
set /a MAP_TRIES+=1
map -w -p %PART% -pr b -o top_map.ncd top.ngd top.pcf && goto map_done
if %MAP_TRIES% lss 3 (
  echo *** map stopped without finishing; trying again
  goto map_again
)
goto fail
:map_done
echo === par (place and route)
par -w top_map.ncd top.ncd top.pcf || goto fail
echo === trce (timing report, build\top.twr)
trce -v 10 top.ncd top.pcf -o top.twr || goto fail
findstr /c:"Timing errors" /c:"timing errors" /c:"Score" top.twr
echo === bitgen
bitgen -w -g StartUpClk:CClk -g CRC:Enable top.ncd top.bit || goto fail
echo Built %HERE%%OUT%\top.bit

if /i "%ACTION%"=="prog" (
  openFPGALoader -b minispartan6 top.bit || goto fail
)
if /i "%ACTION%"=="flash" (
  rem openFPGALoader -f can't reach this board's flash (docs\board.md, SPI flash):
  rem the firmware writes it instead, over the serial port.
  echo openFPGALoader can't write this board's flash. Load the image, then write it with
  echo the firmware over serial:
  echo   build.bat [hdmi] prog
  echo   py tools\modplayer.py --port COMn flash-write %HERE%%OUT%\top.bit
  exit /b 1
)
exit /b 0

:fail
echo.
echo *** Build stopped: see the messages above (logs are in %HERE%%OUT%).
exit /b 1
