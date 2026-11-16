// Behavioral tests for vo_avfoundation's vsync snap (patch 0307), compiled
// verbatim from the final Apple series by test_avf_vsync_snap.sh.
//
// A synthetic display shows each stamp on a refresh by one of the two rules a
// display layer could use: A, the first refresh at or after the stamp; B, the
// refresh nearest to it; each decided with up to 1 ms of compositor jitter.
// Planned times carry Matroska's 1 ms timestamp rounding (and extra jitter
// where noted), as mpv's audio-slaved schedule hands them to the VO. The
// refresh each frame lands on must keep the content's cadence: at the same
// rate, one frame per refresh, never a skip next to a repeat. The pacing of
// enqueues (avf_vsync_pace_ns) must hold them at one point of the refresh
// while mpv's schedule drifts, and auto pacing must find and hold a clean
// enqueue point on each Apple TV's measured map without dwelling in a bad one.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avf_vsync_snap.inc"

#define MS 1000000.0

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

// Uniform in [-1, 1).
static double rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(rng_state >> 11) / (double)(1ull << 52) - 1.0;
}

struct stream {
    double period;      // display refresh period, ns
    double frame;       // content frame duration on the host clock, ns
    double phase;       // planned time of frame 0 past refresh 0, ns
    bool mkv;           // round media times to 1 ms
    double jitter;      // extra planned-time jitter, +- ns
    double noise;       // compositor decision jitter, +- ns
    bool snap;          // stamp through the snap (else the planned time)
    int rule;           // 'A' or 'B'
    double lead;        // stamp lead before its refresh, fraction
    int cut_every;      // a hard cut every ~this many frames (0: none)
};

struct result {
    int frames;
    int step_count[8];  // steps of the shown refresh index, 0..6, 7 = other
    int *steps;         // per frame (frames - 1 entries)
    double max_shift;   // max |shown refresh - planned|, ns
    double sum_shift;
    double min_edge;    // min distance of a stamp to a refresh instant, ns
    double min_mid;     // ... to a refresh's midpoint
    struct avf_vsync_snap s;
};

static int64_t planned(const struct stream *st, int n, double base)
{
    double media = n * st->frame;
    if (st->mkv)
        media = round(media / MS) * MS;
    return (int64_t)llround(base + st->phase + media + st->jitter * rnd());
}

static int64_t shown_index(const struct stream *st, int64_t stamp)
{
    double t = stamp + st->noise * rnd();
    return st->rule == 'A' ? (int64_t)ceil(t / st->period)
                           : (int64_t)floor(t / st->period + 0.5);
}

static double dist_to_grid(double t, double period, double at)
{
    double x = fmod(t - at, period);
    if (x < 0)
        x += period;
    return fmin(x, period - x);
}

// Offset of t past the refresh at or before it (refreshes at multiples of p).
static int64_t vsync_mod_t(int64_t t, int64_t p)
{
    return t - avf_vsync_floor_div(t, p) * p;
}

// base: start of the run on the host clock (a whole number of refreshes).
static void run(const struct stream *st, int frames, struct result *res)
{
    *res = (struct result){0};
    res->frames = frames;
    res->steps = calloc(frames, sizeof(int));
    res->min_edge = res->min_mid = 1e18;
    int64_t period_ns = (int64_t)llround(st->period);
    double base = 1000.0 * period_ns;   // refresh 1000
    int64_t prev = 0;
    for (int n = 0; n < frames; n++) {
        int64_t target = planned(st, n, base);
        int64_t stamp = target;
        // Cuts at random, ~cut_every frames apart.
        bool cut = st->cut_every > 0 && (rnd() + 1) * st->cut_every < 2;
        res->s.cand = (struct avf_step_cand){
            .valid = st->cut_every > 0, .kind = cut ? 1 : 0,
            .repeat = cut ? 40 : 5 + rnd(), .skip = cut ? 40 : 10 + rnd()};
        if (st->snap) {
            // The latest refresh the clock reported: one queue lead earlier.
            int64_t now = target - (int64_t)(150 * MS);
            int64_t vsync = (now / period_ns) * period_ns;
            int64_t refresh = avf_vsync_snap_refresh(&res->s, target, vsync,
                                                     period_ns);
            double shift = (double)(refresh - target);
            res->sum_shift += shift;
            if (fabs(shift) > res->max_shift)
                res->max_shift = fabs(shift);
            stamp = refresh - (int64_t)llround(st->lead * period_ns);
        }
        res->min_edge = fmin(res->min_edge, dist_to_grid(stamp, period_ns, 0));
        res->min_mid = fmin(res->min_mid,
                            dist_to_grid(stamp, period_ns, period_ns / 2.0));
        int64_t idx = shown_index(st, stamp);
        if (n > 0) {
            int64_t step = idx - prev;
            res->steps[n - 1] = (int)step;
            res->step_count[step >= 0 && step < 7 ? step : 7]++;
        }
        prev = idx;
    }
}

static bool all_steps(const struct result *r, int want)
{
    return r->step_count[want] == r->frames - 1;
}

// Content for the long runs: what each frame offers as a hiding place.
enum { C_CUTS, C_PAN, C_STATIC, C_TALK, C_FADES, C_UNREADABLE, C_PAN_LOCAL };
static const char *const content_name[] = {
    "a film (cuts ~4 s apart)", "a pan with no cut for 3 h", "a static shot",
    "talking heads (cuts ~6 s apart)", "a pan with a fade through black every 3 min",
    "frames that cannot be read",
    "no cut for 3 h: 2 min slow pans, 20 s of a still camera with one person moving",
};

// Luma grids for C_PAN_LOCAL, scored as the VO does (avf_grid_score).
#define GN (32 * 18)
static uint8_t grids[3][GN];

static void grid_frame(int n, uint8_t *g)
{
    int seg = n % (24 * 140);
    bool pan = seg < 24 * 120;
    for (int i = 0; i < GN; i++) {
        int x = i % 32, y = i / 32;
        int noise = (int)(1.2 * rnd());
        int v;
        if (pan) {
            // A slow, even pan: every cell's content slides 0.1 cell a frame.
            double phase = x * 0.9 + y * 0.4 + n * 0.1;
            v = 120 + (int)(30 * sin(phase)) + noise;
        } else {
            // A fixed camera: one region (a person, 8% of the picture) moves.
            v = 100 + ((x * 7 + y * 13) % 50) + noise;
            if (x >= 12 && x < 17 && y >= 6 && y < 15)
                v += (int)(25 * sin(n * 0.7 + x + y));
        }
        g[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static struct avf_step_cand content(int c, int n)
{
    struct avf_step_cand k = {.valid = true};
    switch (c) {
    case C_CUTS:
        if ((rnd() + 1) * 96 < 2)
            return (struct avf_step_cand){.valid = true, .kind = 1, .repeat = 40, .skip = 40};
        k.repeat = 5 + rnd();
        k.skip = 10 + 2 * rnd();
        return k;
    case C_PAN:
        k.repeat = 6 + 0.8 * rnd();
        k.skip = 12 + 1.2 * rnd();
        return k;
    case C_STATIC:
        k.repeat = 0.3 + 0.1 * rnd();
        k.skip = 0.35 + 0.1 * rnd();
        return k;
    case C_TALK:
        if ((rnd() + 1) * 144 < 2)
            return (struct avf_step_cand){.valid = true, .kind = 1, .repeat = 35, .skip = 35};
        k.repeat = 2 + rnd();
        k.skip = 3 + 1.5 * rnd();
        return k;
    case C_FADES: {
        int f = n % 4320;   // a 1 s fade through black every 3 minutes
        if (f >= 12 && f < 15)
            k.kind = 2;     // the frame before is black
        k.repeat = f < 24 ? 3 + rnd() : 5 + rnd();
        k.skip = f < 24 ? 6 + rnd() : 10 + 2 * rnd();
        return k;
    }
    case C_PAN_LOCAL:
        memcpy(grids[2], grids[1], GN);
        memcpy(grids[1], grids[0], GN);
        grid_frame(n, grids[0]);
        if (n < 2)
            return (struct avf_step_cand){0};
        k.repeat = avf_grid_score(grids[0], grids[1], GN);
        k.skip = avf_grid_score(grids[0], grids[2], GN);
        return k;
    default:
        return (struct avf_step_cand){0};
    }
}

struct sim_out {
    int steps, by_kind[6], two, none, relocks, forced;
    double av_min, av_max, av_sum, score_max, speed_min, speed_max;
    double typical;         // the content's typical score for that direction
};

// frames of a 23.976 fps film (Matroska rounding, 0.5 ms jitter) on a display
// whose refresh is ppm slower than the film (negative: faster). pcm: the
// sound is decoded, so the servo's speed is applied (cap max_ppm).
static int sim_early_ms;     // the picture-early (sound-late) limit, 0: default
static int step_in_pan;      // C_PAN_LOCAL: steps taken during a pan
static int av_hist[200];     // picture offset histogram, 1 ms bins from -120 ms

static void sim(double ppm, int c, bool pcm, double max_ppm, int frames,
                struct sim_out *o)
{
    memset(av_hist, 0, sizeof(av_hist));
    step_in_pan = 0;
    const double film = 1001.0 / 24.0 * 1e6;
    const int64_t period_ns = (int64_t)llround(film * (1 + ppm * 1e-6));
    struct avf_vsync_snap s = {0};
    struct avf_av_servo v = {0};
    double speed = 1, t = 1000.0 * period_ns + 9 * MS, prev_media = 0;
    int64_t prev_shown = 0, prev_target = 0;
    *o = (struct sim_out){.av_min = 1e18, .av_max = -1e18, .speed_min = 1e9,
                          .speed_max = -1e9};
    double sum_score = 0;
    for (int n = 0; n < frames; n++) {
        double media = round(n * film / MS) * MS;
        t += (media - prev_media) / speed;
        prev_media = media;
        int64_t target = (int64_t)llround(t + 0.5 * MS * rnd());
        int64_t vsync = ((target - (int64_t)(119 * MS)) / period_ns) * period_ns;
        s.cand = content(c, n);
        s.picture_early_ns = (int64_t)sim_early_ms * 1000000;
        bool applied = pcm && s.frames >= AVF_VSYNC_SNAP_ALPHA_DIV &&
                       avf_vsync_one_to_one(&s, (double)period_ns);
        s.hold = applied && !v.saturated;
        int before = s.reslots;
        int64_t shown = avf_vsync_snap_refresh(&s, target, vsync, period_ns);
        if (s.reslots != before) {
            o->score_max = fmax(o->score_max, s.last_score);
            step_in_pan += c == C_PAN_LOCAL && n % (24 * 140) < 24 * 120;
        }
        double score = ppm >= 0 ? s.cand.skip : s.cand.repeat;
        if (s.cand.valid && !s.cand.kind)
            sum_score += score;
        if (applied) {
            double dt = prev_target ? (target - prev_target) / 1e9 : 0;
            speed = avf_av_servo_update(&v, -s.lock_ns / 1e6, dt, max_ppm);
            // The resampling in the last hour (the first minutes pull a
            // fresh lock in).
            if (n >= frames - 24 * 3600) {
                o->speed_min = fmin(o->speed_min, speed);
                o->speed_max = fmax(o->speed_max, speed);
            }
        }
        prev_target = target;
        double av = (double)(shown - target);
        // Lip sync once the lock has settled (and, decoded, the servo has
        // had two minutes to pull a fresh lock in).
        if (n > (pcm ? 24 * 120 : AVF_VSYNC_SNAP_ALPHA_DIV)) {
            o->av_min = fmin(o->av_min, av);
            o->av_max = fmax(o->av_max, av);
            o->av_sum += av;
            int bin = (int)floor(av / MS) + 120;
            av_hist[bin < 0 ? 0 : bin > 199 ? 199 : bin]++;
        }
        if (n > 0) {
            int64_t step = (shown - prev_shown + period_ns / 2) / period_ns;
            if (step != 1) {
                o->steps++;
                o->two += step <= 0;
                o->none += step >= 2;
            }
        }
        prev_shown = shown;
    }
    for (int k = 0; k < 6; k++)
        o->by_kind[k] = s.steps_by_kind[k];
    o->forced = s.steps_by_kind[AVF_STEP_FORCED];
    o->relocks = s.relocks;
    o->typical = sum_score / frames;
}

static double av_pct(double q)
{
    int total = 0, seen = 0;
    for (int i = 0; i < 200; i++)
        total += av_hist[i];
    for (int i = 0; i < 200; i++) {
        seen += av_hist[i];
        if (seen >= q * total)
            return i - 120 + 0.5;
    }
    return 79.5;
}

static int sim_cases(void)
{
    int bad = 0;
    char what[300];
    const int three_h = 24 * 3600 * 3;
    // a. Passthrough on the Master TV (refresh 14 ppm slow): 3 or 4 skips
    //    in 3 hours whatever the content; where each falls.
    for (int c = C_CUTS; c <= C_PAN_LOCAL; c++) {
        struct sim_out o;
        sim(14, c, false, 0, three_h, &o);
        printf("3 h, passthrough, 14 ppm, %s: %d steps (two in a refresh %d, none %d): "
               "cut %d black %d still %d low %d forced %d, worst score %.2f (typical "
               "%.2f); picture %+.1f..%+.1f ms of its sound, mean %+.1f\n",
               content_name[c], o.steps, o.two, o.none, o.by_kind[1], o.by_kind[2],
               o.by_kind[3], o.by_kind[4], o.by_kind[5], o.score_max, o.typical,
               o.av_min / MS, o.av_max / MS, o.av_sum / three_h / MS);
        printf("  picture offset percentiles: 5%% %+.1f, 50%% %+.1f, 95%% %+.1f ms\n",
               av_pct(0.05), av_pct(0.5), av_pct(0.95));
        bool ok = o.steps >= 3 && o.steps <= 4 && o.steps == o.two && o.relocks == 1 &&
                  o.av_min >= -60 * MS - 2 * MS && o.av_max <= 25 * MS + 2 * MS;
        if (c == C_CUTS || c == C_TALK)
            ok = ok && o.by_kind[1] == o.steps;
        else if (c == C_STATIC)
            ok = ok && o.by_kind[3] == o.steps;
        else if (c == C_FADES)
            ok = ok && o.by_kind[2] == o.steps;
        else if (c == C_PAN)
            // In a pan the step lands on one of its calmest frames: within
            // 10% of the lowest score the pan ever has (12 - 1.2).
            ok = ok && o.by_kind[4] == o.steps && o.score_max <= 10.8 * 1.1;
        else if (c == C_PAN_LOCAL) {
            // Never in a pan: each step waits for the still camera.
            printf("  steps during a pan: %d of %d\n", step_in_pan, o.steps);
            ok = ok && step_in_pan == 0 && o.forced == 0;
        } else
            ok = ok && o.forced == o.steps;
        snprintf(what, sizeof(what), "3 h passthrough %s", content_name[c]);
        if (!ok) {
            fprintf(stderr, "FAIL: %s\n", what);
            bad++;
        }
    }
    // a2. The lip-sync range's sound-late side: 60 ms (the default, EBU
    //     R37) against 90 ms (inside ITU-R BT.1359's 125 ms threshold): how
    //     long a step can wait for a hiding place at 14 ppm, and where the
    //     picture sits, for a film.
    for (int ms = 60; ms <= 90; ms += 30) {
        struct sim_out o;
        sim_early_ms = ms;
        sim(14, C_CUTS, false, 0, three_h, &o);
        double window_min = (ms + 25 - 41.708) / 0.014 / 60;
        printf("sound late up to %d ms: a step may wait %.0f min at 14 ppm; a film: "
               "%d steps at cuts; picture %+.1f..%+.1f ms, 5/50/95%%: %+.1f %+.1f %+.1f\n",
               ms, window_min, o.by_kind[1], o.av_min / MS, o.av_max / MS,
               av_pct(0.05), av_pct(0.5), av_pct(0.95));
        if (!(o.by_kind[1] == o.steps && o.av_min >= -(ms + 2) * MS)) {
            fprintf(stderr, "FAIL: range %d ms\n", ms);
            bad++;
        }
    }
    sim_early_ms = 0;

    // b. The same display the other way (refresh 14 ppm fast): repeats.
    {
        struct sim_out o;
        sim(-14, C_CUTS, false, 0, three_h, &o);
        printf("3 h, passthrough, refresh 14 ppm fast, a film: %d steps (repeats: "
               "none in a refresh %d), cut %d forced %d; picture %+.1f..%+.1f ms\n",
               o.steps, o.none, o.by_kind[1], o.forced, o.av_min / MS, o.av_max / MS);
        if (!(o.steps >= 3 && o.steps <= 4 && o.none == o.steps && o.by_kind[1] == o.steps &&
              o.av_min >= -62 * MS && o.av_max <= 27 * MS)) {
            fprintf(stderr, "FAIL: 3 h passthrough, fast refresh\n");
            bad++;
        }
    }
    // c. Clocks locked (the 1st generation Apple TV): no step.
    {
        struct sim_out o;
        sim(0.3, C_CUTS, false, 0, three_h, &o);
        printf("3 h, passthrough, locked clocks, a film: %d steps\n", o.steps);
        if (o.steps) {
            fprintf(stderr, "FAIL: locked clocks stepped\n");
            bad++;
        }
    }
    // d. Decoded sound: resampled to the display, no step at all, the
    //    picture held on its sound (within the first minutes' pull-in); 14
    //    and +-300 ppm (cap 500), locked; and a
    //    mismatch past the cap (24.000 fps on a 23.976 Hz mode, 1001 ppm),
    //    where the steps take the rest.
    struct { double ppm; int content; } d[] = {
        {14, C_PAN}, {300, C_PAN}, {-300, C_PAN}, {0.3, C_PAN}, {1001, C_CUTS},
    };
    for (int i = 0; i < 5; i++) {
        struct sim_out o;
        sim(d[i].ppm, d[i].content, true, 500, three_h, &o);
        printf("3 h, decoded, %+.1f ppm, %s: %d steps (cut %d low %d forced %d); "
               "resample %+.1f..%+.1f ppm; picture %+.2f..%+.2f ms of its sound\n",
               d[i].ppm, content_name[d[i].content], o.steps, o.by_kind[1], o.by_kind[4],
               o.forced, (o.speed_min - 1) * 1e6, (o.speed_max - 1) * 1e6,
               o.av_min / MS, o.av_max / MS);
        bool ok;
        if (fabs(d[i].ppm) <= 300)
            ok = o.steps == 0 && o.av_min >= -8 * MS && o.av_max <= 8 * MS &&
                 fabs((o.speed_min + o.speed_max) / 2 - 1 + d[i].ppm * 1e-6) < 5e-6 &&
                 o.speed_max - o.speed_min < 12e-6;
        else
            // Past the cap: after a minute at it the resampling stops and
            // the steps take it all (one every 42 s), at cuts.
            ok = o.steps >= 250 && o.steps <= 270 && o.forced == 0 &&
                 o.speed_min == 1 && o.speed_max == 1 &&
                 o.av_min >= -62 * MS && o.av_max <= 27 * MS;
        snprintf(what, sizeof(what), "3 h decoded %+.1f ppm", d[i].ppm);
        if (!ok) {
            fprintf(stderr, "FAIL: %s\n", what);
            bad++;
        }
    }
    return bad;
}

// --- auto pacing against a device's map ---
// Drops per hand-over at e ms past the display link's instant (23.976 Hz).
// Apple TV 4K 1st generation (tvOS 26), 2026-11-14 and -16: 8% at 23.5, 7%
// at 32 to 34, about 1 to 3% at the band's edges, none worth the name
// outside. 2nd generation (tvOS 27), 2026-11-15: 10 to 23% from 40 to 8.
static double map_lr(double e)
{
    if (e >= 22 && e < 34)
        return 0.08;
    if ((e >= 19 && e < 22) || (e >= 34 && e < 36.5))
        return 0.02;
    return 0;
}

static double map_master(double e)
{
    if (e >= 40 || e < 8)
        return 0.2;
    if (e >= 38.5)
        return 0.03;
    return 0;
}

static double map_noise(double e)
{
    (void)e;
    return 0.0003;      // a device that drops a frame in 3,300 wherever
}

struct pace_run {
    int moves, heavy, heavy_run, held_at;   // windows; held_at: window it was first held
    long drops;
    double final_ms;
};

// `windows` 5 s windows of 120 frames, each frame handed over at the point
// plus 1 ms of jitter and, one time in `late_every`, 2 to 10 ms late (or,
// `harsh`, 4 to 20 ms: the tail build 48 had on the 1st generation).
static int late_every = 200;
static bool late_harsh;

static struct pace_run pace_run(struct avf_vsync_pace *s, double (*map)(double),
                                int windows)
{
    const double period = 41.708;
    struct pace_run r = {.held_at = -1};
    int run = 0;
    for (int w = 0; w < windows; w++) {
        int dropped = 0;
        for (int i = 0; i < 120; i++) {
            double e = s->phase * period + rnd();
            if ((rnd() + 1) * late_every < 2)
                e += late_harsh ? 12 + 8 * rnd() : 6 + 4 * rnd();
            e = fmod(e + period, period);
            if ((rnd() + 1) / 2 < map(e))
                dropped++;
        }
        r.drops += dropped;
        bool heavy = dropped >= 4;
        run = heavy ? run + 1 : 0;
        r.heavy += heavy;
        if (run > r.heavy_run)
            r.heavy_run = run;
        if (avf_vsync_pace_feed(s, 120 - dropped, dropped) == 1)
            r.moves++;
        if (s->held && r.held_at < 0)
            r.held_at = w;
    }
    r.final_ms = s->phase * period;
    return r;
}

static bool rates_match(const struct avf_vsync_pace *a, const struct avf_vsync_pace *b)
{
    for (int i = 0; i < AVF_PACE_BINS; i++) {
        if (fabs(a->rate[i] - b->rate[i]) > 1e-6)
            return false;
    }
    return true;
}

static int pace_cases(void)
{
    int before = failures;
    char what[200];
    struct {
        const char *name;
        double (*map)(double);
        double start;
        int max_moves, max_heavy, max_heavy_run;
        long max_drops;
    } cases[] = {
        // name, map, start, most moves, most windows with 4+ drops in all
        // and in a row, most frames dropped in four hours (of 345,600)
        {"1st gen from its default", map_lr, 0.05, 0, 0, 0, 40},
        {"2nd gen from the default", map_master, 0.42, 0, 0, 0, 2},
        {"1st gen from the middle (0.42)", map_lr, 0.42, 14, 2, 1, 250},
        {"1st gen from inside its band (0.57)", map_lr, 0.57, 14, 3, 2, 250},
        {"2nd gen from inside its band (0.05)", map_master, 0.05, 14, 3, 2, 300},
        {"2nd gen from its band's edge (0.94)", map_master, 0.94, 14, 3, 2, 300},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        struct avf_vsync_pace s = {0};
        avf_vsync_pace_start(&s, cases[c].start);
        struct pace_run r = pace_run(&s, cases[c].map, 2880);
        double left = cases[c].map(r.final_ms);
        printf("pace %s: %d moves, held from %d s at %.1f ms, %ld frames dropped in "
               "4 h (%d windows with 4 or more, %d in a row)\n", cases[c].name,
               r.moves, r.held_at * 5, r.final_ms, r.drops, r.heavy, r.heavy_run);
        snprintf(what, sizeof(what), "pace %s: held at a clean point, with little "
                 "hunting", cases[c].name);
        check(s.held && left == 0 && r.moves <= cases[c].max_moves &&
              r.heavy <= cases[c].max_heavy && r.heavy_run <= cases[c].max_heavy_run &&
              r.drops <= cases[c].max_drops, what);

        // Saved and read back, the next film starts held there and stays.
        char line[400];
        struct avf_vsync_pace again = {0};
        check(avf_vsync_pace_print(&s, line, sizeof(line)) > 0 &&
              avf_vsync_pace_parse(&again, line) && again.held == s.held &&
              again.rest == s.rest && fabs(again.phase - s.phase) < 1e-4 &&
              again.dir == s.dir && rates_match(&again, &s),
              "pace: the state reads back as written");
        struct pace_run r2 = pace_run(&again, cases[c].map, 720);
        snprintf(what, sizeof(what), "pace %s: the saved point holds", cases[c].name);
        check(r2.moves == 0 && again.held, what);
    }

    // With build 48's late tail (a frame in 50, 4 to 20 ms late) no point
    // near the band is spotless: the search still ends, on a point that
    // drops under a frame in 1,000, without walking into the band.
    {
        late_every = 50;
        late_harsh = true;
        struct avf_vsync_pace s = {0};
        avf_vsync_pace_start(&s, 0.42);
        struct pace_run r = pace_run(&s, map_lr, 2880);
        struct pace_run r2 = pace_run(&s, map_lr, 2880);
        printf("pace 1st gen from the middle, late enqueues: %d moves, %s at %.1f ms, "
               "%ld then %ld frames dropped in 4 h each (%d windows with 4 or more)\n",
               r.moves + r2.moves, s.held ? "held" : s.rest ? "at rest" : "searching",
               r2.final_ms, r.drops, r2.drops, r.heavy + r2.heavy);
        check((s.held || s.rest) && r.moves + r2.moves <= 16 && r.heavy_run <= 2 &&
              r2.drops <= 345, "pace: late enqueues leave it on a good point, at rest");
        late_every = 200;
        late_harsh = false;
    }

    // A steady 0.5% (the 1st generation at 17.2 ms) is acted on within two
    // minutes; a frame in 3,300 everywhere is never taken for a bad point,
    // and what little trying of neighbours it causes ends.
    {
        struct avf_vsync_pace s = {0};
        avf_vsync_pace_start(&s, 0.42);
        int w = 0, moved = 0;
        for (; w < 200 && !moved; w++)
            moved = avf_vsync_pace_feed(&s, 120 - (w % 5 % 2 == 0), w % 5 % 2 == 0) == 1;
        check(moved && w <= 24 && fabs(s.phase - 9.0 / 24) < 1e-9,
              "pace: 0.5% moves the point one bin earlier within two minutes");

        struct avf_vsync_pace n = {0};
        avf_vsync_pace_start(&n, 0.42);
        struct pace_run r = pace_run(&n, map_noise, 8640);
        printf("pace, a frame in 3,300 at every point, 12 h: %d moves, %d windows with "
               "4 or more drops, %s at %.1f ms\n", r.moves, r.heavy,
               n.held ? "held" : n.rest ? "at rest" : "searching", r.final_ms);
        check(r.moves <= 30 && r.heavy == 0 && (n.held || n.rest), "pace: scattered drops end at rest, each bin tried at most once");
    }

    // Too few frames, a negative count, and the window after a move are not
    // judged; a held point that turns bad is left and another found.
    {
        struct avf_vsync_pace s = {0};
        avf_vsync_pace_start(&s, 0.42);
        check(!avf_vsync_pace_feed(&s, 20, 10) && !avf_vsync_pace_feed(&s, 120, -1) &&
              s.frames == 0, "pace: too few frames to judge");
        struct pace_run r = pace_run(&s, map_master, 120);
        check(s.held && r.moves == 0, "pace: held on the 2nd gen");
        r = pace_run(&s, map_lr, 1440);
        check(s.held && map_lr(r.final_ms) == 0 && r.moves >= 1 && r.heavy_run <= 2,
              "pace: a held point that turns bad is left for a clean one");
        check(!avf_vsync_pace_parse(&s, "nonsense") && !avf_vsync_pace_parse(&s, "1.5 0 0 1"),
              "pace: a damaged saved state is refused");
    }
    return failures - before;
}

int main(void)
{
    const double p24 = 1001.0 / 24000.0 * 1e9;   // 23.976 Hz, ns
    char what[256];

    // 1. The fault, and its fix: a 23.976 fps Matroska film on a 23.976 Hz
    //    refresh locked to the audio clock, at every phase across a refresh
    //    (on the boundary exactly and 1 us either side included), both
    //    display rules, 1 ms of compositor jitter.
    int raw_bad_phases = 0, raw_worst = 0;
    for (int rule = 'A'; rule <= 'B'; rule++) {
        for (int i = -2; i <= 400; i++) {
            double phase = i < 0 ? (i == -1 ? 1000.0 : p24 - 1000.0)
                                 : p24 * i / 400.0;
            if (rule == 'B')
                phase = fmod(phase + p24 / 2, p24);  // B's boundary is mid-refresh
            struct stream st = {.period = p24, .frame = p24, .phase = phase,
                                .mkv = true, .noise = 1 * MS, .rule = rule,
                                .lead = 0.25};
            struct result raw, snap;
            run(&st, 2000, &raw);
            int bad = raw.frames - 1 - raw.step_count[1];
            if (bad) {
                raw_bad_phases++;
                if (bad > raw_worst)
                    raw_worst = bad;
            }
            st.snap = true;
            run(&st, 2000, &snap);
            snprintf(what, sizeof(what),
                     "rule %c phase %.3f ms: one frame per refresh (got %d uneven)",
                     rule, phase / MS, snap.frames - 1 - snap.step_count[1]);
            check(all_steps(&snap, 1), what);
            snprintf(what, sizeof(what), "rule %c phase %.3f ms: shift %.2f ms "
                     "within 9/16 refresh plus the rounding", rule, phase / MS,
                     snap.max_shift / MS);
            check(snap.max_shift <= p24 * 9 / 16 + 0.5 * MS, what);
            check(snap.min_edge >= p24 / 4 - 1000 && snap.min_mid >= p24 / 4 - 1000,
                  "stamps sit a quarter refresh from both boundaries");
            check(snap.s.relocks == 1 && snap.s.reslots == 0,
                  "a locked clock never moves the lock");
            free(raw.steps);
            free(snap.steps);
        }
    }
    printf("1:1 23.976: unsnapped, %d of 806 phase/rule cases skip and repeat "
           "(worst %d uneven steps in 2000 frames); snapped, none\n",
           raw_bad_phases, raw_worst);
    check(raw_bad_phases > 0, "the unsnapped model reproduces the fault");

    // 2. Slow drift across the boundary: 23.976 fps on a 24.000 Hz refresh
    //    (a repeat every ~42 s) and 24.000 fps on 23.976 Hz (a skip). Exactly
    //    one uneven step per lock move, never next to another, and the
    //    picture's offset from its planned time averages out.
    for (int dir = 0; dir < 2; dir++) {
        double period = dir ? p24 : 1e9 / 24.0;
        double frame = dir ? 1e9 / 24.0 : p24;
        for (int rule = 'A'; rule <= 'B'; rule++) {
            struct stream st = {.period = period, .frame = frame, .phase = 3 * MS,
                                .mkv = true, .noise = 1 * MS, .rule = rule,
                                .snap = true, .lead = 0.25, .cut_every = 96};
            struct result r;
            int frames = 24 * 60 * 5;   // five minutes
            run(&st, frames, &r);
            int uneven = 0, last_uneven = -1000;
            bool isolated = true, right_kind = true;
            for (int n = 0; n < frames - 1; n++) {
                if (r.steps[n] == 1)
                    continue;
                uneven++;
                isolated = isolated && n - last_uneven > 500;
                right_kind = right_kind && r.steps[n] == (dir ? 0 : 2);
                last_uneven = n;
            }
            snprintf(what, sizeof(what), "drift %s rule %c: %d uneven steps for "
                     "%d lock moves", dir ? "skip" : "repeat", rule, uneven,
                     r.s.reslots);
            // 0.1% drift moves the picture a refresh every 42 s; each step
            // waits for a cut (here ~4 s apart at random) in the second half
            // of its 38 s stretch, none forced.
            check(uneven == r.s.reslots && uneven >= 6 && uneven <= 8 &&
                  r.s.steps_by_kind[AVF_STEP_FORCED] == 0, what);
            check(isolated, "each drift step stands alone");
            check(right_kind, "content slower than the refresh repeats, faster skips");
            double mean = r.sum_shift / frames;
            // Between steps the picture keeps near the middle of its
            // lip-sync range (-60..+25 ms) and never leaves it.
            snprintf(what, sizeof(what), "drift mean shift %.2f ms near the middle "
                     "of the range, max %.2f", mean / MS, r.max_shift / MS);
            check(mean >= -35 * MS && mean <= 0 && r.max_shift <= 60 * MS + 2 * MS, what);
            printf("drift %s rule %c: %d moves in 5 min (%d at a cut, %d forced), "
                   "mean shift %+.2f ms, max %.2f ms\n",
                   dir ? "24.000 on 23.976" : "23.976 on 24.000", rule, r.s.reslots,
                   r.s.steps_by_kind[AVF_STEP_CUT], r.s.steps_by_kind[AVF_STEP_FORCED],
                   mean / MS, r.max_shift / MS);
            free(r.steps);
        }
    }

    // 3. Cadences: 24 fps on 60 Hz keeps a strict 2,3 pulldown; 23.976 on
    //    59.94 too; 25 on 60 a 2,3,2,2,3 (12 refreshes per 5 frames); 30 on
    //    60 and 25 on 50 two refreshes a frame. Every phase, both rules.
    struct { double period, frame; bool mkv; const char *name; } cad[] = {
        {1e9 / 60, 1e9 / 24, true, "24 on 60"},
        {1001e6 / 60000 * 1000, p24, true, "23.976 on 59.94"},
        {1e9 / 60, 1e9 / 25, false, "25 on 60"},
        {1e9 / 60, 1e9 / 30, true, "30 on 60"},
        {1e9 / 50, 1e9 / 25, false, "25 on 50"},
    };
    for (int c = 0; c < 5; c++) {
        int bad = 0;
        for (int rule = 'A'; rule <= 'B'; rule++) {
            for (int i = 0; i <= 200; i++) {
                struct stream st = {.period = cad[c].period, .frame = cad[c].frame,
                                    .phase = cad[c].period * i / 200.0,
                                    .mkv = cad[c].mkv, .jitter = 0.2 * MS,
                                    .noise = 0.5 * MS, .rule = rule,
                                    .snap = true, .lead = 0.25};
                struct result r;
                run(&st, 1200, &r);
                bool ok = true;
                for (int n = 0; n + 1 < r.frames - 1 && ok; n++) {
                    int s0 = r.steps[n], s1 = r.steps[n + 1];
                    if (c <= 1)
                        ok = (s0 == 2 || s0 == 3) && s0 + s1 == 5;
                    else if (c == 2)
                        // While the lock is young (its first 32 frames,
                        // under 1.3 s) one of them may hold a refresh longer
                        // or shorter: 2 or 3 only. After that, 12 refreshes
                        // in every 5 frames.
                        ok = (s0 == 2 || s0 == 3) &&
                             (n < AVF_VSYNC_SNAP_ALPHA_DIV ||
                              n + 5 > r.frames - 1 ||
                              r.steps[n] + r.steps[n + 1] + r.steps[n + 2] +
                              r.steps[n + 3] + r.steps[n + 4] == 12);
                    else
                        ok = s0 == 2;
                }
                bad += !ok;
                free(r.steps);
            }
        }
        snprintf(what, sizeof(what), "%s: the cadence holds at every phase "
                 "(%d of 402 cases broke it)", cad[c].name, bad);
        check(bad == 0, what);
    }
    printf("cadences 24/60, 23.976/59.94, 25/60, 30/60, 25/50 kept at every phase\n");

    // 4. A mode change mid-stream (60 Hz to 23.976 Hz) and a seek (planned
    //    times jump back) each take a new lock; frames after them are even.
    {
        struct avf_vsync_snap s = {0};
        int64_t t60 = (int64_t)llround(1e9 / 60), t24 = (int64_t)llround(p24);
        int64_t target = 0, prev = 0;
        for (int n = 0; n < 50; n++) {
            target = 5000000000LL + (int64_t)llround(n * p24);
            prev = avf_vsync_snap_refresh(&s, target, 0, t60);
        }
        int relocks = s.relocks;
        bool even = true;
        for (int n = 50; n < 300; n++) {
            target = 5000000000LL + (int64_t)llround(round(n * p24 / MS) * MS);
            int64_t refresh = avf_vsync_snap_refresh(&s, target, 7, t24);
            if (n > 50)
                even = even && refresh - prev == t24;
            prev = refresh;
        }
        check(s.relocks == relocks + 1, "a refresh-rate change takes a new lock");
        check(even, "after the mode change, one frame per refresh");
        relocks = s.relocks;
        int64_t back = target - 10000000000LL;
        even = true;
        for (int n = 0; n < 200; n++) {
            int64_t t = back + (int64_t)llround(round(n * p24 / MS) * MS);
            int64_t refresh = avf_vsync_snap_refresh(&s, t, 7, t24);
            if (n > 0)
                even = even && refresh - prev == t24;
            prev = refresh;
        }
        check(s.relocks == relocks + 1, "a seek back takes a new lock");
        check(even, "after the seek, one frame per refresh");
        avf_vsync_snap_unlock(&s);
        avf_vsync_snap_refresh(&s, back + 99000000000LL, 7, t24);
        check(s.relocks == relocks + 2, "an unlock (pause, reset) takes a new lock");
    }

    // 5. No refresh clock: the planned time comes back untouched.
    {
        struct avf_vsync_snap s = {0};
        check(avf_vsync_snap_refresh(&s, 123456789, 0, 0) == 123456789 && !s.locked,
              "without a refresh period the planned time is kept");
    }

    // 6. The lock straddling the boundary: frames planned alternately 0.4 ms
    //    either side of a refresh's midpoint (the worst case of the fault for
    //    rule B, and the snap's own decision point) all go to one refresh.
    {
        struct avf_vsync_snap s = {0};
        int64_t t = (int64_t)llround(p24), prev = 0;
        bool even = true;
        for (int n = 0; n < 1000; n++) {
            int64_t target = 2000000000LL * 0 + n * t + t / 2 +
                             (n & 1 ? 400000 : -400000);
            int64_t refresh = avf_vsync_snap_refresh(&s, target, 0, t);
            if (n > 0)
                even = even && refresh - prev == t;
            prev = refresh;
        }
        check(even && s.reslots == 0,
              "frames alternating across the decision point keep one refresh each");
    }

    // 7. ProMotion and Mac displays. 24 fps on 120 Hz (5 refreshes a frame)
    //    and on 48 Hz (2) at every phase: never 4 or 6 / 1 or 3.
    {
        struct { double period; int want; const char *name; } pm[] = {
            {1e9 / 120, 5, "24 on 120"}, {1e9 / 48, 2, "24 on 48"},
            {1001e6 / 120000 * 1000, 5, "23.976 on 119.88"},
        };
        for (int c = 0; c < 3; c++) {
            int bad = 0, raw_bad = 0;
            for (int rule = 'A'; rule <= 'B'; rule++) {
                for (int i = 0; i <= 200; i++) {
                    struct stream st = {.period = pm[c].period,
                                        .frame = pm[c].want * pm[c].period,
                                        .phase = pm[c].period * i / 200.0,
                                        .mkv = true, .noise = 0.3 * MS,
                                        .rule = rule, .lead = 0.25};
                    struct result r;
                    run(&st, 1500, &r);
                    raw_bad += r.step_count[pm[c].want] != r.frames - 1;
                    free(r.steps);
                    st.snap = true;
                    run(&st, 1500, &r);
                    bad += r.step_count[pm[c].want] != r.frames - 1;
                    free(r.steps);
                }
            }
            snprintf(what, sizeof(what), "%s: %d refreshes a frame at every "
                     "phase (%d of 402 cases broke it)", pm[c].name, pm[c].want, bad);
            check(bad == 0, what);
            printf("%s: unsnapped %d of 402 phase/rule cases uneven, snapped %d\n",
                   pm[c].name, raw_bad, bad);
        }
    }

    // 8. A refresh rate that changes mid-play (ProMotion going 120 -> 60 ->
    //    48 Hz, or a window dragged to a 60 Hz display) takes a new lock each
    //    time, and frames are even on each grid.
    {
        struct avf_vsync_snap s = {0};
        double periods[] = {1e9 / 120, 1e9 / 60, 1e9 / 48, 1e9 / 120};
        int64_t prev = 0;
        bool even = true;
        int n = 0;
        for (int seg = 0; seg < 4; seg++) {
            int64_t t = (int64_t)llround(periods[seg]);
            int64_t phase = 1234567 * (seg + 1) % t;   // each grid its own phase
            for (int k = 0; k < 300; k++, n++) {
                int64_t target = 3000000000LL +
                    (int64_t)llround(round(n * p24 / MS) * MS);
                int64_t vsync = phase + (target / t - 10) * t;
                int64_t refresh = avf_vsync_snap_refresh(&s, target, vsync, t);
                // 23.976 on 120/60/48 Hz: 5, 2-3 or 2 refreshes a frame
                // (with one 1-refresh drift step allowed per segment).
                if (k > AVF_VSYNC_SNAP_ALPHA_DIV) {
                    int64_t step = (refresh - prev + t / 2) / t;
                    even = even && (seg == 1 ? (step == 2 || step == 3)
                                             : (step == (seg == 2 ? 2 : 5) ||
                                                step == (seg == 2 ? 3 : 6)));
                }
                prev = refresh;
            }
        }
        check(s.relocks == 4, "each refresh-rate change takes a new lock");
        check(even, "frames keep their cadence on each refresh rate");
    }

    // 9. A window moved to another display of the same rate: the refreshes
    //    shift by a fraction of a refresh, and the lock is taken again
    //    instead of carrying an offset that now sits on a boundary.
    {
        // 59.94 Hz, so the film's own drift against 60 Hz is not in the way.
        struct avf_vsync_snap s = {0};
        int64_t t = (int64_t)llround(1001e6 / 60000 * 1000), prev = 0;
        bool even = true;
        for (int n = 0; n < 600; n++) {
            int64_t target = 3000000000LL + (int64_t)llround(round(n * p24 / MS) * MS);
            int64_t phase = n < 300 ? 0 : t * 45 / 100;   // moved at frame 300
            int64_t vsync = phase + (target / t - 10) * t;
            int64_t refresh = avf_vsync_snap_refresh(&s, target, vsync, t);
            if (n > 0 && (n < 300 || n > 300 + AVF_VSYNC_SNAP_ALPHA_DIV)) {
                int64_t step = (refresh - prev + t / 2) / t;
                even = even && (step == 2 || step == 3);
            }
            prev = refresh;
        }
        check(s.relocks == 2, "a grid that moved by part of a refresh takes a new lock");
        check(even, "the cadence holds on both displays");
        // Stamps on a slowly re-measured grid (a few us a read) keep the lock.
        struct avf_vsync_snap q = {0};
        for (int n = 0; n < 600; n++) {
            int64_t target = 3000000000LL + (int64_t)llround(round(n * p24 / MS) * MS);
            int64_t vsync = (n % 7) * 3000 + (target / t - 10) * t;
            avf_vsync_snap_refresh(&q, target, vsync, t);
        }
        check(q.relocks == 1, "microseconds of grid noise never take a new lock");
    }

    // 10. Tonight's Master TV (Apple TV 4K, 2nd generation): a 23.976 fps
    //     Matroska film whose audio clock runs 14 ppm off a 41.709 ms
    //     refresh, so the planned times sweep the whole refresh every ~49
    //     minutes. 80,000 frames (55 min) cover a whole sweep and one lock
    //     move, from a lock just below the instant (-3 ms) through it. Each
    //     frame carries Matroska's rounding plus 0.5 ms of jitter. Checked:
    //     one frame per refresh except one clean step per lock move; every
    //     stamp at the same point of its refresh; and the enqueue, which mpv's
    //     schedule puts ~119 ms (+-3) before the planned time, held by the
    //     pacing at one point of the refresh with at least 40 ms of lead,
    //     where unpaced it sweeps the refresh. The two "late zones" are the
    //     enqueue offsets after which the two measured TVs dropped frames
    //     (field logs; a model of what they showed, not of the hardware).
    {
        struct { double frame, period; const char *name; } tv[] = {
            {p24, 41709000.0, "1001/24 ms on 41.709 ms"},
            {41709000.0 * (1 - 14e-6), 41709000.0, "14 ppm on 41.709 ms"},
        };
        for (int c = 0; c < 2; c++) {
            int64_t period_ns = (int64_t)llround(tv[c].period);
            double per = (double)period_ns;
            struct avf_vsync_snap s = {0};
            int frames = 80000, one = 0, two = 0, none = 0, last_step = -100000;
            bool isolated = true, offset_fixed = true, lead_ok = true;
            int late_lr_raw = 0, late_lr = 0, late_m_raw = 0, late_m = 0;
            double enq_min = 1e18, enq_max = -1e18, raw_min = 1e18, raw_max = -1e18;
            double lead_min = 1e18;
            int64_t prev_shown = 0;
            int64_t phase_ns = (int64_t)llround(per * 0.42);
            // Lock -3 ms: planned times 3 ms past the refresh at frame 0.
            double base = 1000.0 * per + 3 * MS;
            for (int n = 0; n < frames; n++) {
                double media = round(n * tv[c].frame / MS) * MS;
                int64_t target = (int64_t)llround(base + media + 0.5 * MS * rnd());
                int64_t now = target - (int64_t)(119 * MS + 3 * MS * rnd());
                int64_t vsync = (now / period_ns) * period_ns;
                int64_t shown = avf_vsync_snap_refresh(&s, target, vsync, period_ns);
                int64_t stamp = shown - (int64_t)llround(0.25 * per);
                double off = (double)vsync_mod_t(stamp, period_ns);
                offset_fixed = offset_fixed && fabs(off - 0.75 * per) <= 1;
                if (n > 0) {
                    int64_t step = (shown - prev_shown + period_ns / 2) / period_ns;
                    if (step == 1) {
                        one++;
                    } else {
                        two += step <= 0;
                        none += step >= 2 ? (int)(step - 1) : 0;
                        isolated = isolated && n - last_step > 1000;
                        last_step = n;
                    }
                }
                prev_shown = shown;
                // Unpaced and paced enqueues, as offsets past the refresh.
                int64_t at = avf_vsync_pace_ns(now, stamp, vsync, period_ns,
                                               phase_ns, (int64_t)(40 * MS));
                double raw_off = (double)vsync_mod_t(now, period_ns);
                double enq_off = (double)vsync_mod_t(at, period_ns);
                lead_min = fmin(lead_min, (double)(stamp - at));
                lead_ok = lead_ok && stamp - at >= (int64_t)(40 * MS);
                raw_min = fmin(raw_min, raw_off);
                raw_max = fmax(raw_max, raw_off);
                enq_min = fmin(enq_min, enq_off);
                enq_max = fmax(enq_max, enq_off);
                // Late zones: 1st generation 30-40 ms, 2nd generation
                // 40.1 ms to 8.1 ms (across the instant).
                late_lr_raw += raw_off >= 30 * MS && raw_off <= 40 * MS;
                late_lr += enq_off >= 30 * MS && enq_off <= 40 * MS;
                late_m_raw += raw_off >= 40.1 * MS || raw_off <= 8.1 * MS;
                late_m += enq_off >= 40.1 * MS || enq_off <= 8.1 * MS;
            }
            printf("tonight, %s: %d frames, refreshes with one frame %d, two %d, "
                   "none %d; lock moves %d, relocks %d\n", tv[c].name, frames,
                   one, two, none, s.reslots, s.relocks);
            printf("  stamp offset fixed at %.2f ms; enqueue offset unpaced %.2f..%.2f "
                   "ms, paced %.3f..%.3f ms, lead >= %.1f ms\n", 0.75 * per / MS,
                   raw_min / MS, raw_max / MS, enq_min / MS, enq_max / MS,
                   lead_min / MS);
            printf("  enqueues in the late zone: 1st gen %d unpaced, %d paced; "
                   "2nd gen %d unpaced, %d paced\n", late_lr_raw, late_lr,
                   late_m_raw, late_m);
            snprintf(what, sizeof(what), "%s: one frame per refresh but for "
                     "one step per lock move (%d steps, %d moves)", tv[c].name,
                     two + none, s.reslots);
            check(two + none == s.reslots && s.reslots >= 1 && s.relocks == 1, what);
            check(isolated, "tonight: each step stands alone");
            check(offset_fixed, "tonight: every stamp at the same point of its refresh");
            check(enq_max - enq_min <= 2, "tonight: paced enqueues at one point");
            check(fabs(enq_min - per * 0.42) <= 2, "tonight: at the pace phase");
            check(lead_ok, "tonight: pacing keeps 40 ms of lead");
            check(raw_max - raw_min > per * 0.9, "tonight: unpaced enqueues sweep the refresh");
            check(late_lr_raw > 0 && late_m_raw > 0 && late_lr == 0 && late_m == 0,
                  "tonight: paced enqueues stay out of both late zones");
        }
    }

    // 11. Pacing on its own: no grid or no wait needed gives now back; a
    //     wait never crosses a refresh; one that would leave less than the
    //     minimum lead is not made. 24 fps on 60 and 120 Hz and 23.976 on
    //     23.976 Hz keep their cadence (pacing never touches the stamps) and
    //     enqueue at one point of the refresh.
    {
        int64_t t = (int64_t)llround(p24);
        check(avf_vsync_pace_ns(5, 1000000000, 0, 0, 0, 0) == 5,
              "pace: no grid, no wait");
        check(avf_vsync_pace_ns(10 * t + 100, 20 * t, 0, t, 100, 0) == 10 * t + 100,
              "pace: already at the point");
        check(avf_vsync_pace_ns(10 * t + 101, 20 * t, 0, t, 100, 0) == 11 * t + 100,
              "pace: the next point, under a refresh away");
        check(avf_vsync_pace_ns(10 * t + 101, 11 * t + 120, 0, t, 100, 40000000) ==
              10 * t + 101, "pace: no wait that leaves under the minimum lead");
        check(avf_vsync_pace_ns(-3 * t - 50, 20 * t, 0, t, 100, 0) == -3 * t + 100,
              "pace: before the grid's instant");
        struct { double period, frame; int want_a, want_b; const char *name; } cad[] = {
            {1e9 / 60, 1e9 / 24, 2, 3, "24 on 60"},
            {1e9 / 120, 1e9 / 24, 5, 5, "24 on 120"},
            {p24, p24, 1, 1, "23.976 on 23.976"},
        };
        for (int c = 0; c < 3; c++) {
            int64_t period_ns = (int64_t)llround(cad[c].period);
            struct avf_vsync_snap s = {0};
            int64_t prev = 0;
            bool ok = true;
            double emin = 1e18, emax = -1e18;
            for (int n = 0; n < 2000; n++) {
                int64_t target = (int64_t)llround(1000.0 * period_ns + 2.3 * MS +
                                                  round(n * cad[c].frame / MS) * MS);
                int64_t now = target - (int64_t)(119 * MS + 3 * MS * rnd());
                int64_t vsync = (now / period_ns) * period_ns;
                int64_t shown = avf_vsync_snap_refresh(&s, target, vsync, period_ns);
                int64_t stamp = shown - period_ns / 4;
                int64_t at = avf_vsync_pace_ns(now, stamp, vsync, period_ns,
                                               (int64_t)llround(period_ns * 0.42),
                                               (int64_t)(40 * MS));
                ok = ok && at >= now && at - now < period_ns && stamp - at >= 40 * MS;
                double e = (double)vsync_mod_t(at, period_ns);
                if (n > AVF_VSYNC_SNAP_ALPHA_DIV) {
                    int64_t step = (shown - prev + period_ns / 2) / period_ns;
                    ok = ok && (step == cad[c].want_a || step == cad[c].want_b);
                    emin = fmin(emin, e);
                    emax = fmax(emax, e);
                }
                prev = shown;
            }
            snprintf(what, sizeof(what), "pace %s: cadence kept, waits under a "
                     "refresh, lead kept, one enqueue point (spread %.3f ms)",
                     cad[c].name, (emax - emin) / MS);
            check(ok && emax - emin <= 2, what);
        }
    }

    // 12. auto: the search for a device's clean enqueue point, against the
    //     two Apple TVs' measured maps (pace_cases()).
    failures += pace_cases();

    // 13: see sim_cases() (content, passthrough and decoded audio).
    failures += sim_cases();

    // 14. The cut and still classifier: a film's frame-to-frame differences
    //     (a few 8-bit steps) are neither; a jump far past them is a cut; a
    //     frozen picture is still; a cut does not raise what is typical.
    {
        double typical = 0;
        int kinds[3] = {0};
        for (int i = 0; i < 2000; i++) {
            double d = 3 + 2 * rnd();
            if (i % 97 == 50)
                d = 40 + 10 * rnd();
            else if (i % 211 == 7)
                d = 0.2;
            kinds[avf_vsync_quiet_kind(d, &typical)]++;
        }
        printf("classifier: %d plain, %d cuts, %d stills (typical diff %.2f)\n",
               kinds[0], kinds[1], kinds[2], typical);
        check(kinds[1] == 21 && kinds[2] == 10 && typical > 2 && typical < 4,
              "classifier: cuts and stills found, motion left alone");
        check(avf_grid_flat(8, 1) && avf_grid_flat(16, 3.5) && avf_grid_flat(120, 1.2) &&
              !avf_grid_flat(16, 6) && !avf_grid_flat(120, 3),
              "black and uniform frames found, dark detail left alone");
    }

    // 15. A cadence that drifts: 23.976 fps on a 60.000 Hz display (no
    //     frame-rate match; 0.1% apart, a 16.7 ms slip of the 2,3 pattern
    //     every ~17 s). Ten minutes of a film with cuts ~4 s apart: every
    //     slip at a cut, none forced, and between slips a strict 2,3.
    {
        struct avf_vsync_snap s = {0};
        int64_t t = (int64_t)llround(1e9 / 60);
        int64_t prev = 0;
        int bad_steps = 0, frames = 24 * 600;
        for (int n = 0; n < frames; n++) {
            int64_t target = 5000000000LL + 7 * (int64_t)MS +
                             (int64_t)llround(round(n * p24 / MS) * MS + 0.3 * MS * rnd());
            int64_t vsync = ((target - (int64_t)(119 * MS)) / t) * t;
            s.cand = content(C_CUTS, n);
            int before = s.reslots;
            int64_t shown = avf_vsync_snap_refresh(&s, target, vsync, t);
            if (n > AVF_VSYNC_SNAP_ALPHA_DIV && s.reslots == before) {
                int64_t step = (shown - prev + t / 2) / t;
                bad_steps += step != 2 && step != 3;
            }
            prev = shown;
        }
        printf("23.976 on 60.000 Hz, 10 min: %d slips (cut %d low %d forced %d), "
               "%d uneven steps outside them\n", s.reslots, s.steps_by_kind[AVF_STEP_CUT],
               s.steps_by_kind[AVF_STEP_LOW], s.steps_by_kind[AVF_STEP_FORCED], bad_steps);
        check(s.reslots >= 30 && s.steps_by_kind[AVF_STEP_FORCED] == 0 && bad_steps == 0 &&
              s.relocks == 1, "cadence: every slip hidden, none forced");
    }

    // 16. A pause and the Apple TV sleeping: playback resumes 30 s later
    //     (planned times jump), the display's refreshes come back at
    //     another phase, and the unpause unlocks. One new lock, then one
    //     frame per refresh again with no step; pacing holds its point.
    {
        struct avf_vsync_snap s = {0};
        int64_t t = (int64_t)llround(41709000.0), prev = 0;
        bool even = true;
        int64_t phase = 0;
        for (int n = 0; n < 2000; n++) {
            if (n == 1000) {
                avf_vsync_snap_unlock(&s);   // VOCTRL_RESUME
                phase = t * 3 / 10;          // the woken display's phase
            }
            int64_t gap = n >= 1000 ? 30000000000LL : 0;
            int64_t target = 3000000000LL + gap + (int64_t)llround(round(n * p24 / MS) * MS);
            int64_t vsync = phase + ((target - phase) / t - 3) * t;
            s.cand = content(C_CUTS, n);
            int rs0 = s.reslots;
            int64_t shown = avf_vsync_snap_refresh(&s, target, vsync, t);
            if (n > 0 && n != 1000 && s.reslots == rs0)
                even = even && (shown - prev + t / 2) / t == 1;
            prev = shown;
            if (n > 1000 + AVF_VSYNC_SNAP_ALPHA_DIV) {
                int64_t at = avf_vsync_pace_ns(target - 119000000, shown - t / 4, vsync, t,
                                               (int64_t)(t * 0.42), 40000000);
                even = even && ((at - phase) % t + t) % t == (int64_t)(t * 0.42);
            }
        }
        // (41.709 ms refreshes, 16 ppm off the film: a drift step may fall
        // in here, at a cut.)
        check(even && s.relocks == 2 && s.reslots == s.steps_by_kind[AVF_STEP_CUT],
              "pause and sleep: one new lock, one frame per refresh, pacing holds");
    }

    // 17. Pacing never makes a frame later than mpv's schedule leaves it:
    //     at a start or a seek a frame due 12 ms ahead goes at once.
    {
        int64_t t = 41709000;
        check(avf_vsync_pace_ns(100 * t + 5, 100 * t + 5 + 12000000, 0, t,
                                (int64_t)(t * 0.42), 40000000) == 100 * t + 5,
              "pace: a frame due soon is not held");
    }

    // 18. The refresh clock: the period from the link's timestamps, through
    //     the link stopping (background, a sleeping display), coming back on
    //     its grid or at another phase, and display mode changes.
    {
        struct avf_refresh_run r = {0};
        const int64_t P = 41709000, N = 41708333;   // measured / link interval
        int64_t t = 1000000000, got = 0;
        for (int i = 0; i < 600; i++, t += P)
            got = avf_refresh_measure(&r, t + (int64_t)(50000 * rnd()), N, true);
        check(llabs(got - P) <= 200, "refresh: a steady link measures its period");
        // Stopped 3 s, back on the same grid: the run goes on.
        t += 72 * P;
        int64_t start = r.start;
        got = avf_refresh_measure(&r, t, N, true);
        check(r.start == start && llabs(got - P) <= 200, "refresh: a gap on the grid keeps the run");
        // Stopped 5 s, back at another phase: a new run, the old period
        // still published, then the new run's own.
        t += 120 * P + P * 3 / 10;
        got = avf_refresh_measure(&r, t, N, true);
        bool kept = r.start == t && llabs(got - P) <= 200;
        for (int i = 0; i < 20; i++)
            got = avf_refresh_measure(&r, t += P, N, true);
        check(kept && llabs(got - P) <= 1000, "refresh: a gap at another phase restarts, period kept");
        // The TV goes to 60 Hz (asleep, the menu) and back.
        int64_t P60 = 16683333;
        got = avf_refresh_measure(&r, t += P60, P60, true);
        bool to60 = got == P60;
        for (int i = 0; i < 30; i++)
            got = avf_refresh_measure(&r, t += P60, P60, true);
        to60 = to60 && llabs(got - P60) <= 200;
        got = avf_refresh_measure(&r, t += P, N, true);
        bool back = got == N;
        for (int i = 0; i < 30; i++)
            got = avf_refresh_measure(&r, t += P, N, true);
        check(to60 && back && llabs(got - P) <= 1000,
              "refresh: a mode change starts over at the new interval");
        struct avf_refresh_run q = {0};
        check(avf_refresh_measure(&q, 5, N, false) == 0,
              "refresh: no period before measuring where the interval is not trusted");
    }

    // 19. Playback through all of that, as the VO drives the snap: the link
    //     stops (frames go unsnapped and the lock is dropped) and comes back
    //     at another phase; a trip to 60 Hz and back; a seek while paused
    //     and the resume. One new lock at each, no step anywhere, one frame
    //     per refresh (2,3 at 60 Hz) once each lock has settled.
    {
        struct avf_vsync_snap s = {0};
        int64_t t24 = 41709000, t60 = 16683333, phase = 0;
        int64_t prev = 0, media0 = 3000000000LL, target = 0;
        int relocks_expected = 1, bad = 0, settle = 0;
        int64_t tp = t24;
        for (int n = 0; n < 6000; n++) {
            bool gap = n >= 1000 && n < 1072;               // link stopped 3 s
            if (n == 1072) {
                phase = t24 * 3 / 10;
                relocks_expected++;
                settle = n + AVF_VSYNC_SNAP_ALPHA_DIV;
            }
            if (n == 2000) {
                tp = t60;
                relocks_expected++;
                settle = n + AVF_VSYNC_SNAP_ALPHA_DIV;
            }
            if (n == 3000) {
                tp = t24;
                phase = 777777;
                relocks_expected++;
                settle = n + AVF_VSYNC_SNAP_ALPHA_DIV;
            }
            if (n == 4000) {                                // pause, seek back 10 min, resume
                avf_vsync_snap_unlock(&s);
                media0 -= 600000000000LL + 30000000000LL;
                relocks_expected++;
                settle = n + AVF_VSYNC_SNAP_ALPHA_DIV;
            }
            target = media0 + (int64_t)llround(round(n * p24 / MS) * MS);
            if (n >= 4000)
                target += 30000000000LL * 21;               // wall time moved on
            if (gap) {
                avf_vsync_snap_unlock(&s);                  // no refresh: planned time used
                prev = 0;
                continue;
            }
            int64_t vsync = phase + ((target - phase) / tp - 3) * tp;
            s.cand = content(C_CUTS, n);
            int rs0 = s.reslots;
            int64_t shown = avf_vsync_snap_refresh(&s, target, vsync, tp);
            if (prev && n > settle && s.reslots == rs0) {
                int64_t step = (shown - prev + tp / 2) / tp;
                bad += tp == t60 ? (step != 2 && step != 3) : step != 1;
            }
            prev = shown;
        }
        // (The refresh here is 41.709 ms against 41.708 ms frames: 16 ppm,
        // so the odd drift step is due; each must fall at a cut.)
        printf("link gap, 60 Hz and back, seek while paused: %d locks (%d expected), "
               "%d drift steps (%d at cuts), %d uneven frames\n", s.relocks,
               relocks_expected, s.reslots, s.steps_by_kind[AVF_STEP_CUT], bad);
        check(s.relocks == relocks_expected && s.steps_by_kind[AVF_STEP_FORCED] == 0 &&
              s.reslots == s.steps_by_kind[AVF_STEP_CUT] && bad == 0,
              "re-anchoring: one lock each, no forced step, even frames");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_avf_vsync_snap: all passed\n");
    return 0;
}
