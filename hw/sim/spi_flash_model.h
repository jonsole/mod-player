// A rule-checking model of the board's SPI flash for testbenches (issue #17),
// behaving like the board's Macronix MX25L6406 (8 MB, JEDEC ID c2 20 17). Call step()
// once per system clock with the pins as the FPGA drives them; it returns
// MISO for the next clock.
//
// SPI mode 0: the flash samples MOSI on SCK's rising edge and changes MISO
// after the falling edge, most significant bit first. Commands:
//   9F read JEDEC ID      05 read status (bit 0 busy, bit 1 write enabled)
//   06 write enable       04 write disable
//   03 read (24-bit address, any length, wraps at the end)
//   02 page program (24-bit address, 1..256 bytes, within one 256-byte page;
//      programming can only clear bits)
//   20 erase 4 KB sector  D8 erase 64 KB block
//   AB release power-down B9 power down
// Program and erase need write enable first, clear it when they finish, and
// keep the chip busy for a while (only 05 is allowed meanwhile).
//
// Broken rules are counted in `failures` and described on stdout: a
// command while busy, program or erase without write enable, a page
// program crossing its page, chip select rising mid-byte, an unknown
// command, a command while powered down, and SCK changing too fast
// (faster than half the system clock: the real chip allows 50 MHz for 03,
// so this only catches glitches).
#ifndef SPI_FLASH_MODEL_H
#define SPI_FLASH_MODEL_H

#include <cstdint>
#include <cstdio>
#include <vector>

struct SpiFlashModel {
    static const uint32_t SIZE = 8u << 20;
    std::vector<uint8_t> mem;
    int failures = 0;
    long long commands = 0;

    // timing, in system clocks, for program and erase (much shorter than the
    // real chip's milliseconds, so tests run quickly; firmware must poll)
    int program_clocks = 2000, sector_erase_clocks = 20000, block_erase_clocks = 40000;

    SpiFlashModel() : mem(SIZE, 0xFF) {}

    int step(int cs_n, int sck, int mosi)
    {
        if (busy_left > 0 && --busy_left == 0) wel = false;
        if (cs_n) {
            if (!was_cs_n) end_of_command();
            was_cs_n = true;
            last_sck = sck;
            return miso;
        }
        if (was_cs_n) {                       // chip select falls: a new command
            was_cs_n = false;
            bit = 0; byte = 0; count = 0; cmd = -1; addr = 0; out = 0xFF;
            if (sck) fail("SCK high when chip select falls (mode 0 starts low)");
        }
        if (sck != last_sck) {
            if (clocks_since_edge < 1) fail("SCK changed on consecutive clocks");
            clocks_since_edge = 0;
            if (sck) rising(mosi & 1);
            else falling();
        } else {
            clocks_since_edge++;
        }
        last_sck = sck;
        return miso;
    }

    bool busy() const { return busy_left > 0; }

private:
    bool was_cs_n = true, wel = false, powered_down = false;
    int last_sck = 0, clocks_since_edge = 10;
    int bit = 0, byte = 0, count = 0, cmd = -1;
    uint32_t addr = 0;
    uint32_t out = 0;       // byte being shifted out
    int miso = 1;
    long long busy_left = 0;
    int pp_bytes = 0;
    uint32_t pp_start = 0;

    void fail(const char *what)
    {
        if (failures++ < 10) printf("SPI flash: %s (command %02x, byte %d)\n", what, cmd & 0xFF, count);
    }

    void rising(int b)
    {
        byte = (byte << 1) | b;
        if (++bit == 8) {
            got_byte(byte & 0xFF);
            bit = 0;
            byte = 0;
            count++;
        }
    }

    void falling()
    {
        // the falling edge after a byte's 8th rising edge puts the next
        // byte's first bit out
        if (bit == 0) out = next_out_byte();
        miso = (out >> 7) & 1;
        out = (out << 1) & 0xFF;
    }

    // what the flash sends during byte `count` (the byte being clocked now)
    int next_out_byte()
    {
        if (cmd == 0x9F) {
            static const uint8_t id[3] = {0xC2, 0x20, 0x17};
            int i = count - 1;
            return i >= 0 && i < 3 ? id[i] : 0xFF;
        }
        if (cmd == 0x05) return (busy() ? 1 : 0) | (wel ? 2 : 0);
        if (cmd == 0x03 && count >= 4) {
            uint8_t v = mem[addr % SIZE];
            addr = (addr + 1) % SIZE;
            return v;
        }
        return 0xFF;
    }

    void got_byte(int v)
    {
        if (count == 0) {
            cmd = v;
            commands++;
            if (powered_down && v != 0xAB) { fail("command while powered down"); return; }
            if (busy() && v != 0x05) { fail("command while busy"); return; }
            switch (v) {
            case 0x9F: case 0x05: case 0x03: case 0x02: case 0x20: case 0xD8: case 0x06: case 0x04:
            case 0xAB: case 0xB9:
                break;
            default:
                fail("unknown command");
            }
            if ((v == 0x02 || v == 0x20 || v == 0xD8) && !wel) fail("program or erase without write enable");
            pp_bytes = 0;
            return;
        }
        if (cmd == 0x03 || cmd == 0x02 || cmd == 0x20 || cmd == 0xD8) {
            if (count <= 3) {
                addr = (addr << 8) | (uint32_t)v;
                if (count == 3) { addr %= SIZE; pp_start = addr; }
                return;
            }
            if (cmd == 0x02) {
                if ((pp_start & 0xFF) + pp_bytes >= 256) { fail("page program crosses its page"); return; }
                pending.push_back({pp_start + pp_bytes, (uint8_t)v});
                pp_bytes++;
            }
        }
    }

    struct Write { uint32_t addr; uint8_t value; };
    std::vector<Write> pending;

    void end_of_command()
    {
        if (bit != 0) fail("chip select rose mid-byte");
        switch (cmd) {
        case 0x06: wel = true; break;
        case 0x04: wel = false; break;
        case 0xB9: powered_down = true; break;
        case 0xAB: powered_down = false; break;
        case 0x02:
            if (wel && count >= 5 && !busy()) {
                for (const Write &w : pending) mem[w.addr] &= w.value;
                busy_left = program_clocks;
            }
            break;
        case 0x20:
        case 0xD8:
            if (wel && count == 4 && !busy()) {
                uint32_t size = cmd == 0x20 ? 4096 : 65536;
                uint32_t base = addr & ~(size - 1);
                for (uint32_t i = 0; i < size; i++) mem[base + i] = 0xFF;
                busy_left = cmd == 0x20 ? sector_erase_clocks : block_erase_clocks;
            } else if (count != 4) {
                fail("erase without exactly 3 address bytes");
            }
            break;
        default:
            break;
        }
        pending.clear();
        cmd = -1;
    }
};

#endif
