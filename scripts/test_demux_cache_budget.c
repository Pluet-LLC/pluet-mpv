// Behavioural test for mpv patch 0320, compiled by test_demux_cache_budget.sh
// from the "demux cache budget" region of demux/demux.c: demux_fw_budget()
// (how far read_packet reads ahead) and demux_bw_budget() (what
// prune_old_packets keeps behind). A model of the packet cache plays a file
// through them as demux.c uses them: the reader reads a packet while the
// forward bytes are under the forward budget, the decoder takes packets
// (they move behind), and pruning drops whole keyframe runs from behind
// while it holds more than the backward budget allows.
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "demux_cache_budget.inc"

#define MIB (UINT64_C(1) << 20)

// Upstream (mpv 0.42) prune_old_packets' budget, for the limit switched off.
static uint64_t upstream_bw(uint64_t max_fw, uint64_t max_bw, bool donate, uint64_t fw)
{
    uint64_t max_avail = max_bw;
    if (max_avail && max_fw > (fw + 1) && donate)
        max_avail += max_fw - (fw + 1);
    return max_avail;
}

struct run {
    uint64_t max_fw, max_bw, total;
    uint64_t pkt, gop;      // packet size, packets per keyframe run
    uint64_t fw_after_start; // forward bytes once the reader first stopped
    uint64_t most_total;     // the most ahead + behind ever held
    uint64_t fw_end, bw_end;
    uint64_t least_bw_after_full; // behind, after it first reached max_bw
};

// fw/bw in packets; pruning drops gop packets at a time.
static void play(struct run *r, int steps)
{
    uint64_t fw = 0, bw = 0, behind_runs_pkts = 0;
    bool started = false, full = false;
    r->most_total = 0;
    r->least_bw_after_full = UINT64_MAX;
    for (int i = 0; i < steps; i++) {
        // Reader: as far as the forward budget allows.
        while (fw * r->pkt < demux_fw_budget(r->max_fw, r->max_bw, r->total, bw * r->pkt)) {
            fw++;
            // prune_old_packets runs as packets are added.
            while (bw && bw * r->pkt > demux_bw_budget(r->max_fw, r->max_bw, r->total, true, fw * r->pkt)) {
                uint64_t drop = behind_runs_pkts < r->gop ? behind_runs_pkts : r->gop;
                if (!drop)
                    drop = bw;
                bw -= drop > bw ? bw : drop;
                behind_runs_pkts = bw;
            }
            uint64_t t = (fw + bw) * r->pkt;
            if (t > r->most_total)
                r->most_total = t;
        }
        if (!started) {
            r->fw_after_start = fw * r->pkt;
            started = true;
        }
        // Decoder: one packet moves behind.
        if (fw) {
            fw--;
            bw++;
            behind_runs_pkts = bw;
        }
        while (bw && bw * r->pkt > demux_bw_budget(r->max_fw, r->max_bw, r->total, true, fw * r->pkt)) {
            uint64_t drop = r->gop < bw ? r->gop : bw;
            bw -= drop;
        }
        if (bw * r->pkt >= r->max_bw && r->max_bw)
            full = true;
        if (full && bw * r->pkt < r->least_bw_after_full)
            r->least_bw_after_full = bw * r->pkt;
    }
    r->fw_end = fw * r->pkt;
    r->bw_end = bw * r->pkt;
}

int main(void)
{
    int checks = 0;

    // Off: upstream's budgets exactly.
    for (uint64_t fw = 0; fw < 64 * MIB; fw += 4093 * 17) {
        for (int d = 0; d < 2; d++) {
            assert(demux_bw_budget(40 * MIB, 10 * MIB, 0, d, fw) == upstream_bw(40 * MIB, 10 * MIB, d, fw));
            assert(demux_bw_budget(40 * MIB, 0, 0, d, fw) == 0);
            assert(demux_fw_budget(40 * MIB, 10 * MIB, 0, fw) == 40 * MIB);
            checks += 3;
        }
    }

    // One budget of 40 MiB, 10 behind, as Pluet sets it (max_fw = total).
    struct run a = {.max_fw = 40 * MIB, .max_bw = 10 * MIB, .total = 40 * MIB,
                    .pkt = 64 * 1024, .gop = 52};
    play(&a, 20000);
    printf("one budget 40/10: after start %.1f MiB ahead; settled %.1f ahead + %.1f behind; most held %.2f MiB\n",
           a.fw_after_start / (double)MIB, a.fw_end / (double)MIB, a.bw_end / (double)MIB,
           a.most_total / (double)MIB);
    assert(a.fw_after_start == 40 * MIB);          // reads ahead into all of it
    assert(a.most_total <= 40 * MIB + a.pkt);      // never more than one packet over
    assert(a.bw_end <= 10 * MIB && a.bw_end + a.gop * a.pkt > 10 * MIB); // keeps max_bw (to a run)
    assert(a.fw_end + a.bw_end <= 40 * MIB + a.pkt);
    assert(a.fw_end >= 30 * MIB - a.pkt);
    // Reading ahead never pushed behind under max_bw less one keyframe run.
    assert(a.least_bw_after_full + a.gop * a.pkt >= 10 * MIB);
    checks += 6;

    // The same sizes, limit off: up to 40 + 10 held.
    struct run b = a;
    b.total = 0;
    play(&b, 20000);
    printf("separate 40 + 10: after start %.1f MiB ahead; settled %.1f ahead + %.1f behind; most held %.2f MiB\n",
           b.fw_after_start / (double)MIB, b.fw_end / (double)MIB, b.bw_end / (double)MIB,
           b.most_total / (double)MIB);
    assert(b.most_total > 40 * MIB + a.pkt);
    checks++;

    // No back buffer (a memory warning's trim): none, ahead up to the limit.
    struct run c = {.max_fw = 48 * MIB, .max_bw = 0, .total = 48 * MIB, .pkt = 64 * 1024, .gop = 52};
    play(&c, 5000);
    assert(c.bw_end == 0 && c.fw_end <= 48 * MIB && c.most_total <= 48 * MIB + c.pkt);
    checks++;

    // A forward limit under the total still caps ahead.
    struct run d = {.max_fw = 20 * MIB, .max_bw = 10 * MIB, .total = 40 * MIB, .pkt = 64 * 1024, .gop = 52};
    play(&d, 20000);
    assert(d.fw_after_start == 20 * MIB && d.most_total <= 40 * MIB + d.pkt);
    checks++;

    // A total under max_bw: never negative, nothing ahead past it.
    assert(demux_fw_budget(40 * MIB, 50 * MIB, 40 * MIB, 45 * MIB) == 0);
    assert(demux_bw_budget(40 * MIB, 50 * MIB, 40 * MIB, true, 41 * MIB) == 40 * MIB);
    checks += 2;

    printf("demux cache budget: %d checks passed\n", checks);
    return 0;
}
