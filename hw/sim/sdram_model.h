// Model of the board's SDRAM (16M x 16: 4 banks, 8192 rows, 512 columns)
// for Verilator testbenches. It stores data, and it referees: any breach of
// the chip's rules (timing, start-up sequence, refresh, bus contention) is
// reported as a failure.
//
// Call step() once per controller clock, after the falling edge (that is
// the SDRAM's rising edge: its clock is the controller clock inverted).
// It samples the command pins and computes dq_in. With real pin delays and
// the chip's access time, read data is valid from just after the next
// rising edge until shortly after the falling edge after that; testbenches
// therefore present dq_in to the controller only for its next FALLING
// edge, and random data at the rising edges (see dq_garbage()). Outside
// read windows dq_in itself is random, so a controller that samples a
// cycle early or late reads garbage.
//
// The controller is expected to use burst length 2, CAS latency 2 and auto
// precharge. Timing limits are for a -6/-7 speed grade part, rounded up to
// whole 20 ns cycles at 50 MHz.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

struct SdramModel {
    static const int T_RCD = 1;           // ACTIVE to READ/WRITE (18 ns)
    static const int T_RP = 1;            // PRECHARGE to ACTIVE/REFRESH (18 ns)
    static const int T_RC = 4;            // ACTIVE to ACTIVE, same bank (63 ns)
    static const int T_RAS = 3;           // ACTIVE to PRECHARGE (42 ns)
    static const int T_WR = 2;            // last write data to PRECHARGE (2 clocks)
    static const int T_RFC = 4;           // REFRESH to anything (66 ns)
    static const int T_MRD = 2;           // MODE REGISTER to anything
    static const int T_RRD = 1;           // ACTIVE to ACTIVE, other bank (12 ns)
    static const int BL = 2, CL = 2;
    static const int INIT_WAIT = 10000;   // 200 us before the first command
    static const int REFRESH_MAX = 390;   // 7.8 us between refreshes

    std::vector<uint16_t> mem = std::vector<uint16_t>(16u << 20);
    long long cycle = 0;
    int failures = 0;
    uint32_t rng = 12345;
    uint16_t dq_in = 0;
    bool driving = false;                 // the chip is outputting read data this cycle
    long long reads = 0, writes = 0, refreshes = 0;

    enum { POWER_UP, PRECHARGED, REFRESHED, READY } phase = POWER_UP;
    int init_refreshes = 0;
    long long last_refresh = -1, mrs_at = -1000, refresh_at = -1000, last_act = -1000;
    long long busy_until = -1;            // a burst's data beats run until here
    struct Bank { bool open = false; int row = 0; long long act_at = -1000, idle_at = -1000; } bank[4];
    struct Beat { long long at; bool read; uint32_t addr; };
    std::vector<Beat> beats;               // data beats still to come

    void fail(const char *what)
    {
        if (failures++ < 10)
            printf("SDRAM FAIL at cycle %lld: %s\n", cycle, what);
    }
    uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 8; }
    uint16_t dq_garbage() { return (uint16_t)rnd(); }

    void write_beat(uint32_t addr, uint16_t dq, int dqm, bool oe)
    {
        if (!oe) fail("write data not driven");
        if (!(dqm & 1)) mem[addr] = (uint16_t)((mem[addr] & 0xFF00) | (dq & 0x00FF));
        if (!(dqm & 2)) mem[addr] = (uint16_t)((mem[addr] & 0x00FF) | (dq & 0xFF00));
    }

    void step(bool cke, bool cs_n, bool ras_n, bool cas_n, bool we_n, int ba, int a, int dqm,
              uint16_t dq_out, bool dq_oe)
    {
        cycle++;
        if (!cke) fail("CKE low");

        // data beats due at this edge
        bool reading = false;
        dq_in = (uint16_t)rnd();
        for (size_t i = 0; i < beats.size();) {
            if (beats[i].at != cycle) { i++; continue; }
            if (beats[i].read) { dq_in = mem[beats[i].addr]; reading = true; }
            else write_beat(beats[i].addr, dq_out, dqm, dq_oe);
            beats.erase(beats.begin() + (long)i);
        }
        if (reading && dq_oe) fail("bus contention: controller drives DQ during read data");
        driving = reading;

        if (cs_n) return;
        int cmd = (ras_n << 2) | (cas_n << 1) | we_n;
        if (cmd == 7) return;             // NOP
        if (phase == POWER_UP && cycle < INIT_WAIT) fail("command before the 200 us power-up wait");
        if (cycle < mrs_at + T_MRD) fail("command too soon after MODE REGISTER (tMRD)");
        if (cycle < refresh_at + T_RFC) fail("command too soon after REFRESH (tRFC)");

        switch (cmd) {
        case 2:                           // PRECHARGE
            if (!(a & 0x400)) { fail("single-bank PRECHARGE (auto precharge expected)"); break; }
            for (auto &b : bank) {
                if (b.open && cycle < b.act_at + T_RAS) fail("PRECHARGE before tRAS");
                if (b.open) b.idle_at = cycle + T_RP;
                b.open = false;
            }
            if (phase == POWER_UP) phase = PRECHARGED;
            break;
        case 1:                           // AUTO REFRESH
            for (auto &b : bank) {
                if (b.open) fail("REFRESH with a bank open");
                if (cycle < b.idle_at) fail("REFRESH before precharge done (tRP)");
            }
            if (phase == POWER_UP) fail("REFRESH before the first PRECHARGE ALL");
            if (phase == PRECHARGED && ++init_refreshes >= 2) phase = REFRESHED;
            if (phase == READY && cycle - last_refresh > REFRESH_MAX) fail("refresh interval over 7.8 us");
            last_refresh = refresh_at = cycle;
            refreshes++;
            break;
        case 0:                           // MODE REGISTER SET
            if (phase != REFRESHED) fail("MODE REGISTER before PRECHARGE ALL and 2 REFRESHes");
            if (ba != 0 || a != 0x021) fail("mode register is not CL2, burst length 2, sequential");
            phase = READY;
            mrs_at = cycle;
            last_refresh = cycle;
            break;
        case 3: {                         // ACTIVE
            Bank &b = bank[ba];
            if (phase != READY) fail("ACTIVE before initialisation finished");
            if (b.open) fail("ACTIVE on an open bank");
            if (cycle < b.idle_at) fail("ACTIVE before precharge done (tRP)");
            if (cycle < b.act_at + T_RC) fail("ACTIVE too soon after ACTIVE, same bank (tRC)");
            if (cycle < last_act + T_RRD) fail("ACTIVE too soon after ACTIVE (tRRD)");
            b.open = true;
            b.row = a & 0x1FFF;
            b.act_at = last_act = cycle;
            break;
        }
        case 5: case 4: {                 // READ / WRITE, with auto precharge
            bool read = cmd == 5;
            Bank &b = bank[ba];
            if (!b.open) { fail("READ/WRITE on a closed bank"); break; }
            if (cycle < b.act_at + T_RCD) fail("READ/WRITE too soon after ACTIVE (tRCD)");
            if (!(a & 0x400)) fail("READ/WRITE without auto precharge");
            if (cycle < busy_until) fail("READ/WRITE during another burst");
            int col = a & 0x1FF;
            if (col & 1) fail("burst starts on an odd column");
            uint32_t base = ((uint32_t)ba << 22) | ((uint32_t)b.row << 9) | (uint32_t)col;
            long long pre;
            if (read) {
                for (int i = 0; i < BL; i++)
                    beats.push_back({cycle + CL + i, true, base + (uint32_t)i});
                busy_until = cycle + BL;
                pre = cycle + BL;          // precharge starts BL clocks after READ
                reads++;
            } else {
                write_beat(base, dq_out, dqm, dq_oe);   // first beat comes with the command
                for (int i = 1; i < BL; i++)
                    beats.push_back({cycle + i, false, base + (uint32_t)i});
                busy_until = cycle + BL;
                pre = cycle + BL - 1 + T_WR;  // tWR after the last data beat
                writes++;
            }
            if (pre < b.act_at + T_RAS) pre = b.act_at + T_RAS;
            b.idle_at = pre + T_RP;
            b.open = false;
            break;
        }
        default:
            fail("unsupported command");
        }
    }

    void finish()
    {
        if (phase != READY) fail("never finished initialisation");
        else if (cycle - last_refresh > REFRESH_MAX) fail("refresh stopped");
    }
};
