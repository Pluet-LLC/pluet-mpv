// See test_hr_seek_budget.sh.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_NOPTS_VALUE (-0x1p+63)

#include "budget.inc"
#include "kf_index.inc"

// The option table entry, against a stand-in for mpv's option macros.
struct opt { const char *name; const char *field; double min, max; };
#define OPT_DOUBLE(f) .field = #f
#define M_RANGE(a, b) .min = (a), .max = (b)
static const struct opt option_table[] = {
#include "option.inc"
};
#define option_entry option_table[0]

static int failures, checks;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            if (failures <= 40) {                               \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
                printf(__VA_ARGS__);                            \
                printf("\n");                                   \
            }                                                   \
        }                                                       \
    } while (0)

static const char *name(enum hr_seek_budget_landing l)
{
    switch (l) {
    case HR_SEEK_BUDGET_EXACT: return "exact";
    case HR_SEEK_BUDGET_BEFORE: return "before";
    case HR_SEEK_BUDGET_AFTER: return "after";
    }
    return "?";
}

// Keyframes every gop seconds from 0 to len: the ones either side of t.
static void grid(double gop, double len, double t, double *before, double *after)
{
    *before = *after = MP_NOPTS_VALUE;
    double k = floor(t / gop) * gop;
    if (k <= t)
        *before = k;
    double a = k == t ? k : k + gop;
    if (a <= len)
        *after = a;
}

static void check_case(double target, double from, double budget,
                       double before, double after,
                       enum hr_seek_budget_landing want)
{
    enum hr_seek_budget_landing got =
        hr_seek_budget_landing(target, from, budget, before, after);
    CHECK(got == want, "target %.3f from %.3f budget %.2f kf %.3f/%.3f: "
          "got %s, want %s", target, from, budget, before, after,
          name(got), name(want));
}

static void test_cases(void)
{
    const double N = MP_NOPTS_VALUE;
    // Within the budget: exact, whatever else.
    check_case(40.0, 30.0, 1.0, 39.5, 50.0, HR_SEEK_BUDGET_EXACT);
    check_case(40.0, 30.0, 1.0, 39.0, 50.0, HR_SEEK_BUDGET_EXACT); // = budget
    check_case(40.0, 30.0, 1.0, 40.0, 40.0, HR_SEEK_BUDGET_EXACT); // on a kf
    // Beyond it: the nearer keyframe.
    check_case(40.0, 30.0, 1.0, 37.0, 47.0, HR_SEEK_BUDGET_BEFORE);
    check_case(40.0, 30.0, 1.0, 34.0, 44.0, HR_SEEK_BUDGET_AFTER);
    check_case(40.0, 30.0, 1.0, 35.0, 45.0, HR_SEEK_BUDGET_BEFORE); // tie
    // +10 s in a file with 10 s GOPs (the plan's "seek to 39 landed at
    // 30.03"): the keyframe before is just ahead of the start, so the one
    // after is nearer.
    check_case(39.0, 29.0, 1.0, 30.03, 40.03, HR_SEEK_BUDGET_AFTER);
    // Forward skip, keyframe before is behind the start: never land there.
    check_case(40.0, 30.0, 1.0, 28.0, 41.5, HR_SEEK_BUDGET_AFTER);
    check_case(40.0, 30.0, 1.0, 30.0, 52.0, HR_SEEK_BUDGET_EXACT);
    // ...nor on the start itself, nor less than half the skip ahead.
    check_case(40.0, 30.0, 1.0, 34.9, 52.0, HR_SEEK_BUDGET_EXACT);
    check_case(40.0, 30.0, 1.0, 35.0, 52.0, HR_SEEK_BUDGET_BEFORE);
    // Backward skip, keyframe after is ahead of the start: never land there.
    check_case(30.0, 40.0, 1.0, 22.0, 41.0, HR_SEEK_BUDGET_EXACT);
    check_case(30.0, 40.0, 1.0, 27.0, 41.0, HR_SEEK_BUDGET_BEFORE);
    check_case(30.0, 40.0, 1.0, 22.0, 34.0, HR_SEEK_BUDGET_AFTER);
    check_case(30.0, 40.0, 1.0, 22.0, 35.1, HR_SEEK_BUDGET_EXACT);
    // Nothing known, or only the keyframe after: exact.
    check_case(40.0, 30.0, 1.0, N, N, HR_SEEK_BUDGET_EXACT);
    check_case(40.0, 30.0, 1.0, N, 40.5, HR_SEEK_BUDGET_EXACT);
    // A "keyframe before" past the target is not one: exact.
    check_case(40.0, 30.0, 1.0, 41.0, 45.0, HR_SEEK_BUDGET_EXACT);
    // Only the keyframe before known (end of the cache/index): it or exact.
    check_case(40.0, 30.0, 1.0, 37.0, N, HR_SEEK_BUDGET_BEFORE);
    check_case(40.0, 30.0, 1.0, 33.0, N, HR_SEEK_BUDGET_EXACT);
    // A seek to where it already is: exact.
    check_case(40.0, 40.0, 1.0, 35.0, 45.0, HR_SEEK_BUDGET_EXACT);
    // A scrub from far away: the nearer keyframe.
    check_case(3000.0, 60.0, 1.0, 2996.0, 3006.0, HR_SEEK_BUDGET_BEFORE);
    check_case(3000.0, 60.0, 1.0, 2992.0, 3002.0, HR_SEEK_BUDGET_AFTER);
    // Short GOPs (a remux, keyframes ~1 s apart) stay exact.
    check_case(40.7, 30.7, 1.0, 40.04, 41.04, HR_SEEK_BUDGET_EXACT);
}

// Every landing over a sweep of starts, skips, GOP lengths and budgets.
static void test_sweep(void)
{
    static const double gops[] = {0.5, 1.001, 2.002, 4.0, 5.005, 10.01, 12.0, 24.0};
    static const double skips[] = {10, -10, 30, -30, 5, -5, 60, 900};
    static const double budgets[] = {0.25, 0.5, 1.0, 2.0, 5.0};
    const double len = 2400;
    int landed[3] = {0};
    for (size_t g = 0; g < sizeof(gops) / sizeof(gops[0]); g++)
    for (size_t k = 0; k < sizeof(skips) / sizeof(skips[0]); k++)
    for (size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++)
    for (double from = 0; from < len; from += 1.37) {
        double gop = gops[g], skip = skips[k], budget = budgets[b];
        double target = from + skip;
        if (target < 0 || target > len)
            continue;
        double before, after;
        grid(gop, len, target, &before, &after);
        enum hr_seek_budget_landing l =
            hr_seek_budget_landing(target, from, budget, before, after);
        landed[l]++;
        double land = l == HR_SEEK_BUDGET_EXACT ? target
                    : l == HR_SEEK_BUDGET_BEFORE ? before : after;
        const char *what = "gop %.3f skip %+.0f budget %.2f from %.2f: %s %.3f";
        // Within the budget it is exact; exact otherwise only if no keyframe
        // qualified.
        if (before != MP_NOPTS_VALUE && target - before <= budget)
            CHECK(l == HR_SEEK_BUDGET_EXACT, what, gop, skip, budget, from,
                  name(l), land);
        // Never on the wrong side of the start; never off by more than
        // half the skip.
        if (skip > 0)
            CHECK(land > from, what, gop, skip, budget, from, name(l), land);
        else
            CHECK(land < from, what, gop, skip, budget, from, name(l), land);
        CHECK(fabs(land - target) <= fabs(skip) / 2 + 1e-9, what, gop, skip,
              budget, from, name(l), land);
        if (l == HR_SEEK_BUDGET_EXACT) {
            bool before_ok = before != MP_NOPTS_VALUE &&
                target - before <= fabs(skip) / 2;
            bool after_ok = after != MP_NOPTS_VALUE &&
                after - target <= fabs(skip) / 2;
            CHECK((before != MP_NOPTS_VALUE && target - before <= budget) ||
                  (!before_ok && !after_ok), what, gop, skip, budget, from,
                  name(l), land);
        } else {
            // The nearer of the two, where both qualify.
            double other = l == HR_SEEK_BUDGET_BEFORE ? after : before;
            if (other != MP_NOPTS_VALUE && fabs(other - target) <= fabs(skip) / 2)
                CHECK(fabs(land - target) <= fabs(other - target), what, gop,
                      skip, budget, from, name(l), land);
        }
    }
    // The sweep must exercise all three outcomes.
    CHECK(landed[0] && landed[1] && landed[2], "sweep outcomes %d/%d/%d",
          landed[0], landed[1], landed[2]);
    printf("sweep: %d exact, %d keyframe before, %d keyframe after\n",
           landed[0], landed[1], landed[2]);
}

// Which seeks the budget decides.
static void test_applies(void)
{
    static const enum seek_type types[] = {
        MPSEEK_NONE, MPSEEK_RELATIVE, MPSEEK_ABSOLUTE, MPSEEK_FACTOR,
        MPSEEK_FRAMESTEP, MPSEEK_CHAPTER,
    };
    static const enum seek_precision precisions[] = {
        MPSEEK_DEFAULT, MPSEEK_KEYFRAME, MPSEEK_EXACT, MPSEEK_VERY_EXACT,
    };
    static const unsigned flagsets[] = {
        0, MPSEEK_FLAG_DELAY, MPSEEK_FLAG_NOFLUSH, MPSEEK_FLAG_BUDGET,
        MPSEEK_FLAG_DELAY | MPSEEK_FLAG_BUDGET,
    };
    static const double budgets[] = {0, 0.5, 1.0};
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++)
    for (size_t p = 0; p < sizeof(precisions) / sizeof(precisions[0]); p++)
    for (size_t f = 0; f < sizeof(flagsets) / sizeof(flagsets[0]); f++)
    for (size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++) {
        struct seek_params seek = {
            .type = types[t], .exact = precisions[p], .amount = 10,
            .flags = flagsets[f],
        };
        bool want = budgets[b] > 0 && (flagsets[f] & MPSEEK_FLAG_BUDGET) &&
                    precisions[p] == MPSEEK_DEFAULT &&
                    (types[t] == MPSEEK_ABSOLUTE || types[t] == MPSEEK_RELATIVE);
        bool got = hr_seek_budget_applies(budgets[b], &seek);
        CHECK(got == want, "type %d precision %d flags %u budget %.1f: %d",
              types[t], precisions[p], flagsets[f], budgets[b], got);
        // Default off: nothing at all with budget 0. +exact is never touched.
        if (budgets[b] == 0 || precisions[p] == MPSEEK_EXACT ||
            precisions[p] == MPSEEK_VERY_EXACT)
            CHECK(!got, "budget %.1f precision %d decided", budgets[b],
                  precisions[p]);
    }
}

static void around(const double *index, int num, double pts,
                   double want_before, double want_after)
{
    double before = MP_NOPTS_VALUE, after = MP_NOPTS_VALUE;
    kf_index_around(index, num, pts, &before, &after);
    CHECK(before == want_before && after == want_after,
          "index around %.3f: %.3f/%.3f, want %.3f/%.3f", pts, before, after,
          want_before, want_after);
}

static void test_index(void)
{
    const double N = MP_NOPTS_VALUE;
    static const double kf[] = {0, 10.01, 20.02, 30.03, 40.04};
    around(kf, 5, 25.0, 20.02, 30.03);
    around(kf, 5, 30.03, 30.03, 30.03);
    around(kf, 5, 0, 0, 0);
    around(kf, 5, 45.0, 40.04, N);
    around(kf, 5, -1.0, N, 0);
    around(kf, 0, 5.0, N, N);
    around(kf, 1, 5.0, 0, N);
    // Values already known (from the cache) are kept.
    double before = 21.0, after = MP_NOPTS_VALUE;
    kf_index_around(kf, 5, 25.0, &before, &after);
    CHECK(before == 21.0 && after == 30.03, "cache values kept: %f/%f",
          before, after);
    before = MP_NOPTS_VALUE, after = 26.0;
    kf_index_around(kf, 5, 25.0, &before, &after);
    CHECK(before == 20.02 && after == 26.0, "cache values kept: %f/%f",
          before, after);
}

static void test_option(void)
{
    CHECK(strcmp(option_entry.name, "hr-seek-budget") == 0, "option name %s",
          option_entry.name);
    CHECK(strcmp(option_entry.field, "hr_seek_budget") == 0, "option field %s",
          option_entry.field);
    CHECK(option_entry.min == 0 && option_entry.max == 3600,
          "option range %f-%f", option_entry.min, option_entry.max);
}

int main(void)
{
    test_cases();
    test_sweep();
    test_applies();
    test_index();
    test_option();
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("hr-seek-budget: %d checks passed\n", checks);
    return 0;
}
