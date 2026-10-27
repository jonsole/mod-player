// The scope capture's reference (hw/rtl/scopes.v, module scope_capture), line
// for line: per Voice, per audio frame, which points it writes. The hardware
// takes a few dozen clocks to write them, so they are queued here and each
// hardware write is checked against the next one. Include once per testbench.
//
// A capture records from its start, a column every 256 frames, into a ring
// of 316; once 158 columns are in, the next rising zero crossing (or the frame
// TIMEOUT later) is the trigger, and it goes on to the 158th column from the
// one at or after the trigger. `first` is then the window's first column in
// the ring: the trigger's column is drawn in the middle.
#ifndef SCOPE_REF_H
#define SCOPE_REF_H

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <utility>

struct ScopeRef {
    int phase = 0;                              // 0 idle, 1 before the trigger, 2 after it, 3 done
    bool begun = false;
    uint32_t t = 0, age = 0, off = 0;           // t: the frame's time, frames since the first point
    bool pending = false;
    int q0 = 0, j = 0;                          // j: columns written, counting on past the ring
    int stop = 1023, first = 0;
    std::deque<std::pair<int, int>> expect;     // (index in the ring, point)
};

static const uint32_t SCOPE_PRE = 158 * 256, SCOPE_TIMEOUT = 36000;

static int scope_q(int v) { return (3 * v) >> 5; }        // the output in 1/16 pixel

// One audio frame for one Voice: its output now (val) and the frame before's (prev).
static void scope_frame(ScopeRef &s, int val, int prev)
{
    if (s.phase != 1 && s.phase != 2) return;
    if (!s.begun) {                             // the first point
        s.begun = true;
        s.expect.push_back({0, scope_q(val) >> 4});
        s.q0 = scope_q(val);
        s.age = 0;
        s.off = 256;
        s.pending = false;
        s.j = 1;
        s.t = 1;
        return;
    }
    uint32_t t = s.t++;
    if (s.phase == 1 && ((prev < 0 && val >= 0 && t >= SCOPE_PRE) || t == SCOPE_PRE + SCOPE_TIMEOUT)) {
        int col = (int)((t + 255) >> 8);        // the column at or after the trigger
        s.phase = 2;
        s.stop = col + 157;
        s.first = col - 158;
    }
    bool chg = val != prev;
    bool stalled = s.age == (val == 0 ? 63u : 1099u);
    if ((s.age >= 63 && (chg || s.pending)) || stalled) {   // a knot: draw the segment
        uint32_t gap = s.age + 1;
        int q1 = scope_q(val), dq = q1 - s.q0;
        uint32_t quo = ((uint32_t)std::abs(dq) << 12) / gap;
        int64_t slope = dq < 0 ? -(int64_t)quo : (int64_t)quo;
        uint32_t c = s.off;
        while (c < gap) {
            s.expect.push_back({s.j >= 316 ? s.j - 316 : s.j, (s.q0 + (int)((slope * (int64_t)c) >> 12)) >> 4});
            if (s.j == s.stop) { s.phase = 3; break; }
            s.j++;
            c += 256;
        }
        if (s.phase != 3) s.off = c - gap;
        s.q0 = q1;
        s.age = 0;
        s.pending = false;
    } else {
        s.age++;
        if (chg) s.pending = true;
    }
}

#endif
