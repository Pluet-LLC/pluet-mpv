// Tests for vo_avfoundation's subtitle refresh match (patch 0312), compiled
// verbatim from the final Apple series by test_avf_subref.sh. A synthetic
// 23.976 Hz display, one frame a refresh; the OSD ring gets a subtitle change
// on some frames, and a look names the video and OSD buffers shown.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avf_subref.inc"
#include "avf_restart_stamp.inc"

static int failures, checks;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#define P 41708333LL

// Buffers are identities only.
static char vb[100], ob[100], empty_a, empty_b;

static int look(struct avf_subref_ring *vid, struct avf_subref_ring *osd,
                const void *vshown, const void *oshown, int64_t now, bool *known)
{
    struct avf_subref_entry *v = avf_subref_find(vid, vshown, now);
    *known = false;
    if (!v)
        return 0;
    struct avf_subref_entry *due = avf_subref_due(osd, v->stamp_ns);
    struct avf_subref_entry *shown = avf_subref_find(osd, oshown, v->stamp_ns);
    if (!due || !shown)
        return 0;
    *known = true;
    return avf_subref_judge(v->stamp_ns, shown, due, P);
}

int main(void)
{
    struct avf_subref_ring vid = {0}, osd = {0};
    // Frames 0..9 stamped one refresh apart; subtitle A from frame 3, B from 6.
    for (int i = 0; i < 10; i++) {
        int64_t st = 1000 * P + i * P;
        avf_subref_push(&vid, &vb[i], st, st - 2 * P, false);
        if (i == 0)
            avf_subref_push(&osd, &empty_a, st, st - 2 * P, true);
        if (i == 3)
            avf_subref_push(&osd, &ob[3], st, st - 2 * P, false);
        if (i == 6)
            avf_subref_push(&osd, &ob[6], st, st - 2 * P, false);
    }
    bool k;
    int64_t now = 1000 * P + 5 * P;
    check(look(&vid, &osd, &vb[4], &ob[3], now, &k) == 0 && k, "in step mid-subtitle");
    check(look(&vid, &osd, &vb[3], &ob[3], now, &k) == 0 && k, "in step on the change");
    check(look(&vid, &osd, &vb[3], &empty_a, now, &k) == -1 && k, "a refresh late on the change");
    check(look(&vid, &osd, &vb[4], &empty_a, now, &k) == -2 && k, "two refreshes late");
    check(look(&vid, &osd, &vb[2], &ob[3], now, &k) == 1 && k, "a refresh early");
    check(look(&vid, &osd, &vb[1], &ob[3], now, &k) == 2 && k, "two refreshes early");
    check(look(&vid, &osd, &vb[6], &ob[3], now, &k) == -1 && k, "late on the second change");
    check(look(&vid, &osd, &vb[0], &empty_a, now, &k) == 0 && k, "transparent before the first");
    // An unknown buffer is unjudged.
    look(&vid, &osd, &vb[50], &ob[3], now, &k);
    check(!k, "unknown video buffer");
    look(&vid, &osd, &vb[4], &ob[50], now, &k);
    check(!k, "unknown osd buffer");

    // The same subtitle buffer handed over again (a resend): still in step.
    avf_subref_push(&osd, &ob[6], 1000 * P + 8 * P, 0, false);
    check(look(&vid, &osd, &vb[8], &ob[6], now, &k) == 0 && k, "resent buffer");

    // Two different transparent samples count as the same picture.
    struct avf_subref_ring o2 = {0};
    avf_subref_push(&o2, &empty_a, 1000 * P, 0, true);
    avf_subref_push(&o2, &empty_b, 1000 * P + 2 * P, 0, true);
    check(look(&vid, &o2, &vb[3], &empty_a, now, &k) == 0 && k, "transparent equals transparent");

    // A flush at frame 5's stamp: the change queued for frame 6 never shows,
    // so subtitle A is what is due from then on.
    struct avf_subref_ring o3 = osd;
    avf_subref_flush_after(&o3, 1000 * P + 5 * P);
    check(look(&vid, &o3, &vb[7], &ob[3], now, &k) == 0 && k, "flushed change not due");

    // A sample shown at once replaces everything before it.
    struct avf_subref_ring o4 = osd;
    avf_subref_push(&o4, &empty_b, AVF_SUBREF_AT_ONCE, 0, true);
    check(look(&vid, &o4, &vb[8], &empty_b, now, &k) == 0 && k, "clear shown at once is due");
    check(look(&vid, &o4, &vb[8], &ob[6], now, &k) == 0 && !k, "replaced sample unjudged");

    // The ring keeps the newest AVF_SUBREF_RING entries.
    struct avf_subref_ring r = {0};
    for (int i = 0; i < AVF_SUBREF_RING + 10; i++)
        avf_subref_push(&r, &vb[i % 100], i, i, false);
    check(r.count == AVF_SUBREF_RING && avf_subref_at(&r, 0)->stamp_ns == AVF_SUBREF_RING + 9,
          "ring wraps, newest first");
    check(avf_subref_find(&r, &vb[0], 0) == NULL, "oldest gone after wrap");

    // The lead model.
    check(!avf_subref_predict(1.3, 1.1, false), "both under, same refresh");
    check(avf_subref_predict(1.3, 1.1, true), "both under, an instant between");
    check(avf_subref_predict(3.0, 2.8, false), "one in the band");
    check(avf_subref_predict(3.5, 2.5, false), "different zones");
    check(!avf_subref_predict(4.0, 3.6, true), "both over: honoured");

    // The restart stamp (0313): a refresh under 3.5 refreshes past the
    // hand-over moves on by whole refreshes; one past it stays.
    {
        int64_t vs = 0, st = 1 * P + P * 3 / 4;   // 75% into refresh 1
        int64_t moved = avf_host_restart_stamp_ns(st, 0, vs, P);
        check((moved - st) % P == 0 && moved > st, "restart: moved by whole refreshes");
        int64_t refresh = moved + P - (moved % P);
        check(refresh >= P * 7 / 2 && refresh - P < P * 7 / 2, "restart: first refresh past 3.5");
        check(avf_host_restart_stamp_ns(10 * P + 1, 0, vs, P) == 10 * P + 1, "restart: far enough stays");
        check(avf_host_restart_stamp_ns(st, 0, vs, 0) == st, "restart: no period, no change");
    }

    printf("test_avf_subref: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
