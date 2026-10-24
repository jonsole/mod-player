// Verilog Paula vs the Paula C model (ticket #10, ADR-0005/0009).
//
// Both get the same register writes, a tick at a time; every output frame
// the Verilog Paula produces must equal the model's frame for the same
// colour clock. Memory answers Paula's fetches after a random delay, so the
// output can't depend on fetch timing.
//
//   paula_tb song.mod seconds options   (Replayer drives both)
//   paula_tb --fuzz seed ticks          (random register writes)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "Vpaula.h"
#include "verilated.h"

// (Verilator compiles the C sources as C++, so no extern "C" here.)
#include "mod.h"
#include "paula_model.h"
#include "replayer.h"

struct Write { uint32_t reg, value; };

static Vpaula *dut;
static paula_model_t model;
static std::vector<uint8_t> mem;        // the Verilog Paula's memory
static std::vector<uint8_t> model_mem;  // the model's own copy
static std::deque<paula_frame_t> expected;
static uint64_t cycles, frames_checked;
static uint32_t rng = 1;
static int failures;

static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }

// Memory: answers a fetch request after 0..15 cycles.
static int mem_wait = -1;

static void tick_clock()
{
    dut->clk = 0;
    dut->eval();
    // memory model (sampled on the falling edge, driven for the rising edge)
    dut->mem_ack = 0;
    if (dut->mem_req) {
        if (mem_wait < 0)
            mem_wait = (int)(rnd() % 16);
        if (mem_wait-- == 0) {
            uint32_t a = (uint32_t)dut->mem_addr << 1;
            if (dut->mem_we) {
                if ((dut->mem_be & 1) && a < mem.size()) mem[a] = (uint8_t)dut->mem_wdata;
                if ((dut->mem_be & 2) && a + 1 < mem.size()) mem[a + 1] = (uint8_t)(dut->mem_wdata >> 8);
            } else {
                uint8_t lo = a < mem.size() ? mem[a] : 0, hi = a + 1 < mem.size() ? mem[a + 1] : 0;
                dut->mem_rdata = (uint16_t)(lo | hi << 8);
            }
            dut->mem_ack = 1;
            mem_wait = -1;
        }
    }
    dut->clk = 1;
    dut->eval();
    cycles++;
    if (dut->out_valid) {
        if (expected.empty()) {
            if (failures++ < 5)
                printf("FAIL: Verilog Paula produced a frame the model didn't (frame %llu)\n",
                       (unsigned long long)frames_checked);
            return;
        }
        paula_frame_t e = expected.front();
        expected.pop_front();
        int16_t l = (int16_t)dut->out_l, r = (int16_t)dut->out_r;
        if (l != e.left || r != e.right) {
            if (failures++ < 5)
                printf("FAIL: frame %llu: Verilog (%d, %d), model (%d, %d)\n",
                       (unsigned long long)frames_checked, l, r, e.left, e.right);
        }
        frames_checked++;
    }
}

static void bus_write(uint32_t reg, uint32_t value)
{
    dut->reg_we = 1;
    dut->reg_addr = reg;
    dut->reg_wdata = value;
    tick_clock();
    dut->reg_we = 0;
}

static uint32_t bus_read(uint32_t reg)
{
    dut->reg_addr = reg;
    dut->eval();
    return dut->reg_rdata;
}

// One tick: queue the writes in Paula, give it the tick length, and let the
// model apply them and run the same number of colour clocks.
static void run_tick(const std::vector<Write> &writes, uint32_t cck, bool first)
{
    if (!first) { // wait for the previous boundary, as the firmware does
        while (!(bus_read(PAULA_STATUS) & PAULA_STATUS_TICK))
            tick_clock();
        bus_write(PAULA_STATUS, PAULA_STATUS_TICK);
    }
    for (const Write &w : writes) {
        bus_write(w.reg, w.value);
        paula_model_write(&model, w.reg, w.value);
    }
    bus_write(PAULA_TICK_LEN, cck);
    if (first)
        bus_write(PAULA_CONTROL, 1);
    for (uint32_t i = 0; i < cck; i++)
        expected.push_back(paula_model_step(&model));
}

static void finish(const char *what)
{
    // let Paula catch up with the model
    for (int guard = 0; !expected.empty() && guard < 200000000; guard++)
        tick_clock();
    uint32_t status = bus_read(PAULA_STATUS);
    if (status & (PAULA_STATUS_QUEUE_OVERFLOW)) {
        printf("FAIL: write queue overflowed\n");
        failures++;
    }
    if (status & PAULA_STATUS_BEHIND) {
        printf("FAIL: Paula fell behind real time\n");
        failures++;
    }
    if (mem != model_mem) {
        printf("FAIL: memory differs after INVERT writes\n");
        failures++;
    }
    printf("%s: %llu frames compared, %llu cycles (%.1f per frame): %s\n", what,
           (unsigned long long)frames_checked, (unsigned long long)cycles,
           frames_checked ? (double)cycles / frames_checked : 0.0, failures ? "FAIL" : "ok");
}

static std::vector<Write> pending;
static void record(void *, uint32_t reg, uint32_t value) { pending.push_back({reg, value}); }

static int run_song(const char *path, double seconds, uint32_t options)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot read %s\n", path); return 2; }
    std::vector<uint8_t> file(0x1000000);
    size_t size = fread(file.data(), 1, file.size(), f);
    fclose(f);
    uint32_t null_addr = ((uint32_t)size + 1) & ~1u;
    mem.assign(null_addr + 131072, 0);
    memcpy(mem.data(), file.data(), size);
    static mod_song_t song;
    if (mod_load(&song, mem.data(), (uint32_t)size) != MOD_OK) { printf("bad MOD\n"); return 2; }
    model_mem = mem;
    paula_model_reset(&model, model_mem.data(), (uint32_t)model_mem.size());

    static replayer_t rp;
    pending.clear();
    pending.push_back({PAULA_OPTIONS, options});
    replayer_start(&rp, &song, mem.data(), 0, null_addr, paula_port_t{record, nullptr});
    uint64_t total = (uint64_t)(seconds * PAULA_CCK_HZ), done = 0;
    bool first = true;
    while (done < total && !rp.stopped) {
        uint32_t cck = replayer_tick(&rp);
        std::vector<Write> w;
        w.swap(pending);
        run_tick(w, cck, first);
        first = false;
        done += cck;
    }
    char what[512];
    snprintf(what, sizeof what, "%s (options %u)", path, options);
    finish(what);
    return failures ? 1 : 0;
}

static int run_fuzz(uint32_t seed, int ticks)
{
    rng = seed;
    mem.resize(4096);
    for (auto &b : mem)
        b = (uint8_t)rnd();
    model_mem = mem;
    paula_model_reset(&model, model_mem.data(), (uint32_t)model_mem.size());
    static const uint32_t per_choices[] = {0, 1, 112, 113, 114, 127, 200, 428, 856, 1000, 65535};
    for (int t = 0; t < ticks; t++) {
        std::vector<Write> w;
        int n = (int)(rnd() % 12);
        for (int i = 0; i < n; i++) {
            uint32_t v = rnd() % 4, kind = rnd() % 10;
            switch (kind) {
            case 0: w.push_back({PAULA_VOICE(v) + PAULA_PTR, rnd() % 4200}); break;
            case 1: w.push_back({PAULA_VOICE(v) + PAULA_LEN, rnd() % 4 == 0 ? 0 : rnd() % 300}); break;
            case 2: w.push_back({PAULA_VOICE(v) + PAULA_PER, per_choices[rnd() % 11] | (rnd() % 8 == 0 ? 0x10000u : 0)}); break;
            case 3: w.push_back({PAULA_VOICE(v) + PAULA_VOL, rnd() % 256}); break;
            case 4: case 5: w.push_back({PAULA_DMACON, (rnd() % 2 ? PAULA_DMACON_SET : 0) | (rnd() % 16)}); break;
            case 6: w.push_back({PAULA_LED_FILTER, rnd() % 2}); break;
            case 7: w.push_back({PAULA_OPTIONS, rnd() % 8}); break;
            case 8: w.push_back({PAULA_INVERT, rnd() % 4200}); break;
            default: break;
            }
        }
        run_tick(w, 50 + rnd() % 3000, t == 0);
    }
    char what[64];
    snprintf(what, sizeof what, "fuzz seed %u", seed);
    finish(what);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    dut = new Vpaula;
    dut->rst = 1;
    for (int i = 0; i < 4; i++)
        tick_clock();
    dut->rst = 0;
    int rc;
    if (argc == 4 && strcmp(argv[1], "--fuzz") == 0)
        rc = run_fuzz((uint32_t)atoi(argv[2]), atoi(argv[3]));
    else if (argc == 4)
        rc = run_song(argv[1], atof(argv[2]), (uint32_t)atoi(argv[3]));
    else {
        printf("usage: paula_tb song.mod seconds options | paula_tb --fuzz seed ticks\n");
        rc = 2;
    }
    delete dut;
    return rc;
}
