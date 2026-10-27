// The scope capture's reference (hw/rtl/scopes.v, module scope_capture), line
// for line: per Voice, per audio frame, which points it writes. The hardware
// takes a few dozen clocks to write them, so they are queued here and each
// hardware write is checked against the next one. Include once per testbench.
#ifndef SCOPE_REF_H
#define SCOPE_REF_H

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <utility>

struct ScopeRef {
    int phase = 0;                              // 0 idle, 1 waiting, 2 capturing, 3 done
    uint32_t waited = 0, age = 0, off = 0;
    bool pending = false;
    int q0 = 0, j = 0;
    std::deque<std::pair<int, int>> expect;     // (index, point)
};

static int scope_q(int v) { return (3 * v) >> 5; }        // the output in 1/16 pixel

// One audio frame for one Voice: its output now (val) and the frame before's (prev).
static void scope_frame(ScopeRef &s, int val, int prev)
{
    if (s.phase == 1) {
        if ((prev < 0 && val >= 0) || s.waited == 36000) {
            s.phase = 2;
            s.expect.push_back({0, scope_q(val) >> 4});
            s.q0 = scope_q(val);
            s.age = 0;
            s.off = 256;
            s.pending = false;
            s.j = 1;
        }
        s.waited++;
    } else if (s.phase == 2) {
        bool chg = val != prev;
        bool stalled = s.age == (val == 0 ? 63u : 1099u);
        if ((s.age >= 63 && (chg || s.pending)) || stalled) {   // a knot: draw the segment
            uint32_t gap = s.age + 1;
            int q1 = scope_q(val), dq = q1 - s.q0;
            uint32_t quo = ((uint32_t)std::abs(dq) << 12) / gap;
            int64_t slope = dq < 0 ? -(int64_t)quo : (int64_t)quo;
            uint32_t c = s.off;
            while (c < gap) {
                s.expect.push_back({s.j, (s.q0 + (int)((slope * (int64_t)c) >> 12)) >> 4});
                s.j++;
                c += 256;
                if (s.j == 316) { s.phase = 3; break; }
            }
            if (s.phase == 2) s.off = c - gap;
            s.q0 = q1;
            s.age = 0;
            s.pending = false;
        } else {
            s.age++;
            if (chg) s.pending = true;
        }
    }
}

#endif
