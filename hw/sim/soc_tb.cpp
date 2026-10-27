// Runs the SoC against the SDRAM model and a serial line, passing when the
// expected text appears on the serial output.
//
//   soc_tb [options] "expected text"
//     --max N           give up after N clock cycles (default 60 million)
//     --after TEXT      wait for TEXT on the serial output before acting
//     --send FILE       then send FILE's bytes to the board's serial input
//                       (--after/--send pairs can repeat: each step waits
//                       for its text after the previous step acted, and the
//                       expected text must appear after the last step)
//     --preload FILE    then put FILE's bytes at the start of SDRAM (a
//                       back door, so long Songs need not crawl over serial)
//     --switches N      DIP switch setting (bit 0 = SW1)
//     --flash-fill B    start with every byte of the SPI flash model set to B
//                       (default 0xff, erased)
//     --flash-image F   then put F's bytes at the start of the flash model
//     --sd-image F      a microSD card (hw/sim/spi_sd_model.h) with F as its
//                       contents (whole 512-byte blocks); without it no card
//                       is in the socket. --sd-sdsc: a standard-capacity card
//                       (byte addresses); --sd-v1: a version 1 card (no CMD8);
//                       --sd-polls N: ready after N ACMD41s (default 3)
//     --no-console      the external bus answers at once and reads 0, as
//                       the plain build's; otherwise it acts like the HDMI
//                       build's text console (ADR-0012): present, blanking
//                       at 60 Hz, each write taking 10 clocks to accept
//     --compare FILE S  when the firmware starts a Song (it writes Paula's
//                       options), compare Paula's audio with the PC reference
//                       (Replayer + Paula C model) for FILE, for S seconds;
//                       passes when they match (no expected text needed)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "Vsoc.h"
#include "verilated.h"
#include "sdram_model.h"
#include "spi_flash_model.h"
#include "spi_sd_model.h"
#include "mod.h"
#include "paula_model.h"
#include "replayer.h"

static const int CLK_HZ = 50000000, BAUD = 1000000, BIT = CLK_HZ / BAUD;

static std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> d;
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    int c;
    while ((c = fgetc(f)) != EOF)
        d.push_back((uint8_t)c);
    fclose(f);
    return d;
}

// The PC reference for --compare: what `render` would produce.
struct Reference {
    std::vector<uint8_t> mem;
    mod_song_t song;
    paula_model_t paula;
    replayer_t rp;
    std::deque<paula_frame_t> frames;
    bool started = false, silent = false, play_once = false;

    void start(const std::vector<uint8_t> &file, uint32_t options)
    {
        uint32_t silence = ((uint32_t)file.size() + 3) & ~3u;
        mem.assign(silence + 131072 + 4, 0);
        std::copy(file.begin(), file.end(), mem.begin());
        if (mod_load(&song, mem.data(), (uint32_t)file.size()) != MOD_OK) {
            printf("reference: not a MOD file\n");
            exit(2);
        }
        paula_model_reset(&paula, mem.data(), (uint32_t)mem.size());
        paula_model_write(&paula, PAULA_OPTIONS, options);
        replayer_start(&rp, &song, mem.data(), 0, silence, paula_model_port(&paula));
        frames.clear();
        started = true;
        silent = false;
    }
    paula_frame_t next()
    {
        while (frames.empty()) {
            uint32_t cck = 70940;              // the firmware's silent tick
            if ((rp.stopped || (play_once && rp.ended)) && !silent) {  // voices off at the next tick
                paula_model_write(&paula, PAULA_DMACON, 0xF);
                for (int v = 0; v < PAULA_VOICES; v++)
                    paula_model_write(&paula, PAULA_VOICE(v) + PAULA_VOL, 0);
                silent = true;
            } else if (!silent) {
                cck = replayer_tick(&rp);
            }
            for (uint32_t i = 0; i < cck; i++)
                frames.push_back(paula_model_step(&paula));
        }
        paula_frame_t f = frames.front();
        frames.pop_front();
        return f;
    }
};

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    long long max_cycles = 60000000LL;
    const char *expect = nullptr;
    struct Step { const char *after; std::vector<uint8_t> send; };
    std::vector<Step> steps;
    std::vector<uint8_t> preload;
    int switches = 0, flash_fill = 0xFF;
    static SpiSdModel sd;
    bool console = true;
    int console_busy = 0;
    long long console_writes = 0;
    std::vector<uint8_t> flash_image;
    std::vector<uint8_t> compare_song;
    long long compare_frames = 0, compared = 0;
    static Reference ref;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--max") && i + 1 < argc) max_cycles = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--after") && i + 1 < argc) steps.push_back({argv[++i], {}});
        else if (!strcmp(argv[i], "--send") && i + 1 < argc) {
            if (steps.empty())
                steps.push_back({nullptr, {}});
            steps.back().send = read_file(argv[++i]);
        }
        else if (!strcmp(argv[i], "--preload") && i + 1 < argc) preload = read_file(argv[++i]);
        else if (!strcmp(argv[i], "--switches") && i + 1 < argc) switches = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-console")) console = false;
        else if (!strcmp(argv[i], "--flash-fill") && i + 1 < argc) flash_fill = (int)strtol(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--flash-image") && i + 1 < argc) flash_image = read_file(argv[++i]);
        else if (!strcmp(argv[i], "--sd-image") && i + 1 < argc) { sd.image = read_file(argv[++i]); sd.present = true; }
        else if (!strcmp(argv[i], "--sd-sdsc")) sd.sdhc = false;
        else if (!strcmp(argv[i], "--sd-v1")) { sd.v2 = false; sd.sdhc = false; }
        else if (!strcmp(argv[i], "--sd-polls") && i + 1 < argc) sd.acmd41_polls = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compare") && i + 2 < argc) {
            compare_song = read_file(argv[++i]);
            compare_frames = (long long)(atof(argv[++i]) * PAULA_CCK_HZ);
        }
        else if (argv[i][0] != '-' || !argv[i][1]) expect = argv[i];
        else { printf("unknown option %s\n", argv[i]); return 2; }
    }
    if (!expect && compare_frames)
        expect = "\x01never";
    if (!expect) {
        printf("usage: soc_tb [--max N] [--after TEXT] [--send FILE] [--preload FILE] "
               "[--switches N] \"expected text\"\n");
        return 2;
    }

    Vsoc *dut = new Vsoc;
    static SdramModel sdram;
    static SpiFlashModel flash;
    std::string text;
    int rx_timer = -1, rx_index = 0, rx_byte = 0;    // decoding the board's output
    std::deque<uint8_t> tx_queue;                    // bytes for the board's input
    int tx_timer = 0, tx_bit = -1, tx_byte = 0;
    size_t step = 0, acted_at = 0;     // next step to act; where the text stood when the last one did
    if (!steps.empty() && !steps[0].after) {
        tx_queue.assign(steps[0].send.begin(), steps[0].send.end());
        step = 1;
    }
    dut->rst = 1;
    dut->uart_rx = 1;
    dut->card_miso = 1;
    dut->ext_ready = 1;
    dut->ext_rdata = 0;
    dut->switches = switches;
    std::fill(flash.mem.begin(), flash.mem.end(), (uint8_t)flash_fill);
    std::copy(flash_image.begin(), flash_image.begin() + std::min(flash_image.size(), flash.mem.size()),
              flash.mem.begin());

    for (long long cycle = 0; cycle < max_cycles; cycle++) {
        if (cycle == 10)
            dut->rst = 0;
        if (step < steps.size() && text.find(steps[step].after, acted_at) != std::string::npos) {
            if (step == 0)
                for (size_t i = 0; i < preload.size(); i++) {
                    uint16_t &w = sdram.mem[i >> 1];   // the model's word index is the byte address / 2
                    w = (i & 1) ? (uint16_t)((w & 0x00FF) | preload[i] << 8)
                                : (uint16_t)((w & 0xFF00) | preload[i]);
                }
            tx_queue.insert(tx_queue.end(), steps[step].send.begin(), steps[step].send.end());
            acted_at = text.size();
            step++;
        }

        // serial input: 8N1, one byte straight after another
        if (tx_bit < 0 && !tx_queue.empty()) {
            tx_byte = tx_queue.front();
            tx_queue.pop_front();
            tx_bit = 0;
            tx_timer = BIT;
        }
        if (tx_bit >= 0) {
            dut->uart_rx = tx_bit == 0 ? 0 : tx_bit <= 8 ? (tx_byte >> (tx_bit - 1)) & 1 : 1;
            if (--tx_timer == 0) {
                tx_timer = BIT;
                if (++tx_bit == 10)
                    tx_bit = -1;
            }
        } else {
            dut->uart_rx = 1;
        }

        // read data is valid around our falling edge only (see sdram_model.h)
        dut->sd_dq_i = sdram.dq_in;
        dut->clk = 0;
        dut->eval();
        sdram.step(dut->sd_cke, dut->sd_cs_n, dut->sd_ras_n, dut->sd_cas_n, dut->sd_we_n,
                   dut->sd_ba, dut->sd_a, dut->sd_dqm, dut->sd_dq_o, dut->sd_dq_oe);
        dut->sd_dq_i = sdram.dq_garbage();
        if (console) {
            // 640x480p60 at 25.2 MHz is 420000 pixel clocks a frame, 125 : 63
            // to ours; the last 45 of its 525 lines are the blanking
            long long t = (cycle * 63) % (420000LL * 125);
            bool blanking = t >= 480LL * 800 * 125;
            uint32_t frames = (uint32_t)((cycle * 63) / (420000LL * 125));
            if (console_busy > 0) console_busy--;
            bool write = dut->ext_valid && dut->ext_wstrb != 0;
            dut->ext_ready = dut->ext_valid && !(write && console_busy > 0);
            dut->ext_rdata = 0x80000000u | (blanking ? 0x40000000u : 0) | (frames & 0xFFFFFF);
            if (write && dut->ext_ready) { console_busy = 10; console_writes++; }
        }
        dut->clk = 1;
        dut->eval();
        dut->flash_miso = flash.step(dut->flash_cs_n, dut->flash_sck, dut->flash_mosi);
        dut->card_miso = sd.step(dut->card_cs_n, dut->card_sck, dut->card_mosi);
        if (sd.failures) {
            printf("serial: %s\nFAIL: SD card rules broken\n", text.c_str());
            return 1;
        }
        if (flash.failures) {
            printf("serial: %s\nFAIL: SPI flash rules broken\n", text.c_str());
            return 1;
        }
        if (sdram.failures) {
            printf("serial: %s\nFAIL: SDRAM rules broken\n", text.c_str());
            return 1;
        }

        // --compare: a Song starts when the firmware sets Paula's options
        if (compare_frames && dut->dbg_paula_we && dut->dbg_paula_addr == PAULA_OPTIONS) {
            ref.play_once = switches & 4;      // SW3
            ref.start(compare_song, dut->dbg_paula_wdata);
            compared = 0;
        }
        if (ref.started && dut->audio_valid) {
            paula_frame_t e = ref.next();
            int16_t l = (int16_t)dut->audio_l, r = (int16_t)dut->audio_r;
            if (l != e.left || r != e.right) {
                printf("serial: %s\nFAIL: frame %lld: chip (%d, %d), reference (%d, %d)\n",
                       text.c_str(), compared, l, r, e.left, e.right);
                return 1;
            }
            if (++compared == compare_frames) {
                sdram.finish();
                printf("%s: %lld frames match the PC reference (%.1f s of audio), %lld cycles, "
                       "%lld console writes\n", sdram.failures ? "FAIL" : "ok", compared,
                       compared / (double)PAULA_CCK_HZ, cycle, console_writes);
                return sdram.failures ? 1 : 0;
            }
        }

        // serial output: sample each bit in its middle
        if (rx_timer < 0) {
            if (!dut->uart_tx) {
                rx_timer = BIT + BIT / 2;
                rx_index = 0;
                rx_byte = 0;
            }
        } else if (--rx_timer == 0) {
            if (rx_index < 8) {
                rx_byte |= (dut->uart_tx & 1) << rx_index++;
                rx_timer = BIT;
            } else {
                if (!dut->uart_tx)
                    printf("FAIL: framing error after byte 0x%02x\n", rx_byte);
                text += (char)rx_byte;
                rx_timer = -1;
                if (step == steps.size() && text.find(expect, acted_at) != std::string::npos) {
                    sdram.finish();
                    printf("serial: %s\n%s: found \"%s\" after %lld cycles (SDRAM: %lld reads, "
                           "%lld writes, %lld refreshes)\n", text.c_str(),
                           sdram.failures ? "FAIL" : "ok", expect, cycle, sdram.reads,
                           sdram.writes, sdram.refreshes);
                    delete dut;
                    return sdram.failures ? 1 : 0;
                }
            }
        }
    }
    printf("serial: %s\nFAIL: \"%s\" not seen in %lld cycles\n", text.c_str(), expect, max_cycles);
    delete dut;
    return 1;
}
