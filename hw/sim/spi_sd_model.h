// A rule-checking model of a microSD card in SPI mode for testbenches
// (issue #25). Call step() once per system clock with the pins as the FPGA
// drives them (chip select, SCK, MOSI = the card's CMD pin); it returns MISO
// (the card's DAT0) for the next clock. Without a card (present = false) it
// returns 1, a floating pulled-up line.
//
// SPI mode 0, as the SD Physical Layer spec's SPI mode: the card samples MOSI
// on SCK's rising edge and changes MISO after the falling edge, most
// significant bit first. Commands are 6 bytes (0x40 | index, a 32-bit
// argument, CRC7 << 1 | 1); the response (R1, plus 4 bytes for R3/R7) follows
// after `ncr` idle bytes. Supported, as in an initialisation and a read:
//   CMD0   go idle (CRC 0x95 checked)     CMD8  interface condition (v2 only;
//   CMD16  block length (512)                   CRC 0x87 checked, echoes the argument)
//   CMD17  read a block                    CMD55 the next command is an ACMD
//   CMD58  read the OCR                    ACMD41 start up (HCS in bit 30)
// The card is ready after `acmd41_polls` ACMD41s. SDHC cards address blocks,
// SDSC (and v1) cards bytes. A read answers R1, then `token_delay` idle
// bytes, the token 0xFE, 512 bytes and two CRC bytes (0, not checked in SPI
// mode).
//
// Broken rules are counted in `failures` and described on stdout: fewer
// than 74 clocks with chip select high before the first command; SCK faster
// than 400 kHz (assuming a 50 MHz system clock) before the card is ready;
// a command before CMD0; a command other than those above; ACMD41 or a read
// before the card is ready (or CMD58 before CMD0); a bad CRC on CMD0 or
// CMD8; chip select rising in the middle of a byte.
#ifndef SPI_SD_MODEL_H
#define SPI_SD_MODEL_H

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

struct SpiSdModel {
    // configuration
    bool present = false;
    bool v2 = true, sdhc = true;
    int acmd41_polls = 3;                       // ACMD41s answered "idle" before "ready"
    int ncr = 1, token_delay = 2;               // idle bytes before a response / a data token
    std::vector<uint8_t> image;                 // the card's contents (whole blocks)

    int failures = 0;
    long long reads = 0;
    long long commands = 0;

    uint32_t blocks() const { return (uint32_t)(image.size() / 512); }

    int step(int cs_n, int sck, int mosi)
    {
        clocks_since_rise++;
        if (!present) return 1;
        if (cs_n) {
            if (!was_cs_n && bits != 0) fail("chip select rose in the middle of a byte");
            if (!was_cs_n) { out.clear(); cmd_bytes = 0; }          // the card lets go of the bus
            if (sck && !last_sck) {
                rising_edge_checks();
                if (!started) powerup_edges++;
            }
            was_cs_n = true;
            last_sck = sck;
            return 1;
        }
        if (was_cs_n) {                                             // chip select falls
            was_cs_n = false;
            bits = 0;
            rx = 0;
            tx = 0xFF;
            tx_bit = 7;
            miso = 1;
            if (sck) fail("SCK high when chip select falls (mode 0 starts low)");
        }
        if (sck && !last_sck) {                                     // rising edge: sample MOSI
            rising_edge_checks();
            rx = (uint8_t)(rx << 1 | (mosi & 1));
            if (++bits == 8) {
                bits = 0;
                byte_received(rx);
                rx = 0;
                tx = out.empty() ? 0xFF : out.front();
                if (!out.empty()) out.pop_front();
                tx_bit = 7;
            }
        } else if (!sck && last_sck) {                              // falling edge: the next MISO bit
            miso = (tx >> tx_bit) & 1;
            if (tx_bit > 0) tx_bit--;
        }
        last_sck = sck;
        return miso;
    }

private:
    bool was_cs_n = true, last_sck = 0, started = false, idle = false, spi = false, ready = false;
    bool app_cmd = false, hcs = false;
    int bits = 0, tx_bit = 7, miso = 1, powerup_edges = 0, polls = 0, cmd_bytes = 0;
    uint8_t rx = 0, tx = 0xFF, frame[6];
    long long clocks_since_rise = 1000000;
    std::deque<uint8_t> out;

    void fail(const char *why)
    {
        if (failures++ < 10) printf("SD card: %s\n", why);
    }

    void rising_edge_checks()
    {
        // a card that isn't ready yet needs 100-400 kHz: SCK rising edges at
        // least 125 system clocks (50 MHz) apart
        if (!ready && clocks_since_rise < 125)
            fail("SCK faster than 400 kHz before the card is ready");
        clocks_since_rise = 0;
    }

    static uint8_t crc7(const uint8_t *p, int n)
    {
        uint8_t crc = 0;
        for (int i = 0; i < n; i++)
            for (int b = 7; b >= 0; b--) {
                uint8_t bit = (p[i] >> b & 1) ^ (crc >> 6 & 1);
                crc = (uint8_t)((crc << 1) & 0x7F);
                if (bit) crc ^= 0x09;
            }
        return crc;
    }

    void byte_received(uint8_t b)
    {
        if (cmd_bytes == 0 && (b & 0xC0) != 0x40)
            return;                                                 // an idle byte between commands
        frame[cmd_bytes++] = b;
        if (cmd_bytes < 6) return;
        cmd_bytes = 0;
        command(frame[0] & 0x3F, (uint32_t)frame[1] << 24 | (uint32_t)frame[2] << 16 | frame[3] << 8 | frame[4], frame);
    }

    void r1(uint8_t v)
    {
        for (int i = 0; i < ncr; i++) out.push_back(0xFF);
        out.push_back(v);
    }

    void command(int idx, uint32_t arg, const uint8_t *f)
    {
        commands++;
        if (!started) {
            started = true;
            if (powerup_edges < 74) fail("fewer than 74 clocks with chip select high before the first command");
        }
        bool acmd = app_cmd;
        app_cmd = false;
        if (!spi && idx != 0) {
            fail("a command before CMD0");
            return;                                                 // the card isn't in SPI mode: no answer
        }
        uint8_t state = ready ? 0x00 : 0x01;
        if (acmd && idx == 41) {
            if (ready) { r1(0x00); return; }
            if (v2 && (arg >> 30 & 1)) hcs = true;
            if (++polls > acmd41_polls) ready = true;
            r1(ready ? 0x00 : 0x01);
            return;
        }
        if (acmd) {
            r1(0x05);
            fail("an ACMD this model doesn't know");
            return;
        }
        switch (idx) {
        case 0:
            if (f[5] != 0x95) fail("bad CRC on CMD0");
            spi = true;
            ready = false;
            polls = 0;
            r1(0x01);
            break;
        case 8:
            if (f[5] != 0x87 && crc7(f, 5) << 1 != (f[5] & 0xFE)) fail("bad CRC on CMD8");
            if (!v2) { r1(0x05); break; }                           // illegal command: a v1 card
            r1(state);
            out.push_back(0x00); out.push_back(0x00);
            out.push_back((uint8_t)(arg >> 8 & 0x0F)); out.push_back((uint8_t)arg);
            break;
        case 16:
            r1(arg == 512 ? state : 0x40);
            break;
        case 17:
            read_block(arg);
            break;
        case 55:
            app_cmd = true;
            r1(state);
            break;
        case 58: {
            uint32_t ocr = (ready ? 0x80000000u : 0) | (ready && sdhc && hcs ? 0x40000000u : 0) | 0x00FF8000u;
            r1(state);
            for (int i = 3; i >= 0; i--) out.push_back((uint8_t)(ocr >> (8 * i)));
            break;
        }
        default:
            fail("a command this model doesn't know");
            r1(0x05);
        }
    }

    void read_block(uint32_t arg)
    {
        if (!ready) {
            fail("a read before the card is ready");
            r1(0x05);
            return;
        }
        bool block_addressed = sdhc && hcs;
        uint32_t block = block_addressed ? arg : arg / 512;
        if ((!block_addressed && arg % 512 != 0) || block >= blocks()) {
            r1(0x40);                                               // parameter error: no data
            return;
        }
        reads++;
        r1(0x00);
        for (int i = 0; i < token_delay; i++) out.push_back(0xFF);
        out.push_back(0xFE);
        for (int i = 0; i < 512; i++) out.push_back(image[(size_t)block * 512 + i]);
        out.push_back(0x00);
        out.push_back(0x00);
    }
};

#endif
