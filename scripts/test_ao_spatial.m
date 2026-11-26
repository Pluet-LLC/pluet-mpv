// Unit test for the "spatial" audio output
// (patches/mpv/pool/0039-ao-spatial-render-truehd-atmos-and-dts-x.patch).
// It drives the driver the way mpv's push thread does, against a real
// synchronizer and a fake sink. Run scripts/test_ao_spatial.sh, which builds
// it against the patched mpv source.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/out/ao_spatial.m"

// The parts of mpv the driver calls.
static int reloads, fallbacks;
void mp_msg(struct mp_log *log, int lev, const char *format, ...) {}
void ao_request_reload(struct ao *ao) { reloads++; }
void ao_request_pcm_fallback(struct ao *ao) { fallbacks++; }
bool af_fmt_is_spdif(int format) { return format >= AF_FORMAT_S_AAC && format < AF_FORMAT_COUNT; }
const char *af_fmt_to_str(int format) { return "spdif"; }
int af_fmt_to_bytes(int format) { return format == AF_FORMAT_FLOAT ? 4 : 2; }
// Force Dolby MAT's layout choice: the 7.1.4 speakers are offered, and a
// layout made of them only is kept as it is.
static uint64_t offered_speakers;
static int chmap_adjusts;
void mp_chmap_sel_add_speaker(struct mp_chmap_sel *s, int id) { offered_speakers |= UINT64_C(1) << id; }
bool ao_chmap_sel_adjust(struct ao *ao, const struct mp_chmap_sel *s, struct mp_chmap *map)
{
    chmap_adjusts++;
    for (int i = 0; i < map->num; i++) {
        if (!(offered_speakers & (UINT64_C(1) << map->speaker[i])))
            return false;
    }
    return true;
}
const m_option_type_t m_option_type_choice, m_option_type_bool;

struct fake_sink {
    bool refuse;
    int codec;
    bool lossless;
    int layout;
    // Force Dolby MAT: the channels and their 7.1.4 slots it was opened with.
    int pcm_channels;
    int8_t slots[16];
    size_t buffer_limit;
    AVSampleBufferRenderSynchronizer *synchronizer;
    int begins, ends, closes, volumes, failure_reads;
    float volume;
    int64_t begin_time;
    // Bytes the sink takes before it is full.
    size_t space;
    NSMutableData *received;
    bool ready, final;
    int failure;
    int64_t covered;
    // Enqueued audio a route change flushed from the renderer.
    int64_t missing;
};

static struct fake_sink *opening_sink;

void *mp_spatial_create(int codec, bool lossless, int channels, void *synchronizer, size_t buffer_limit)
{
    struct fake_sink *s = opening_sink;
    if (!s) return NULL;
    s->codec = codec;
    s->lossless = lossless;
    s->layout = channels;
    s->synchronizer = synchronizer;
    s->buffer_limit = buffer_limit;
    return s->refuse ? NULL : s;
}
void *mp_spatial_create_pcm(const int8_t *slots, int channels, void *synchronizer, size_t buffer_limit)
{
    struct fake_sink *s = opening_sink;
    if (!s) return NULL;
    s->codec = 0;
    s->pcm_channels = channels;
    memcpy(s->slots, slots, channels);
    s->synchronizer = synchronizer;
    s->buffer_limit = buffer_limit;
    return s->refuse ? NULL : s;
}
void mp_spatial_begin(void *ctx, int64_t time)
{
    struct fake_sink *s = ctx;
    s->begins++;
    s->begin_time = time;
    s->ready = s->final = false;
    s->failure = MP_SPATIAL_OK;
    s->covered = INT64_MIN;
    s->missing = 0;
    [s->received setLength:0];
}
// Like the real sink, a failed timeline takes nothing until the next begin.
size_t mp_spatial_write(void *ctx, const void *data, size_t size)
{
    struct fake_sink *s = ctx;
    size_t taken = s->failure ? 0 : MPMIN(size, s->space);
    s->space -= taken;
    [s->received appendBytes:data length:taken];
    return taken;
}
void mp_spatial_end(void *ctx) { ((struct fake_sink *)ctx)->ends++; }
void mp_spatial_destroy(void *ctx) { ((struct fake_sink *)ctx)->closes++; }
void mp_spatial_volume(void *ctx, float volume)
{
    struct fake_sink *s = ctx;
    s->volumes++;
    s->volume = volume;
}
int mp_spatial_poll(void *ctx, bool *ready, int64_t *covered, int64_t *end, bool *final)
{
    struct fake_sink *s = ctx;
    *ready = s->ready;
    *covered = s->covered;
    *end = s->covered == INT64_MIN ? INT64_MIN : s->covered + s->missing;
    *final = s->final;
    return s->failure;
}
char *mp_spatial_failure(void *ctx)
{
    struct fake_sink *s = ctx;
    s->failure_reads++;
    return s->failure ? strdup("fake failure") : NULL;
}

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define MS INT64_C(1000000)
#define BURST_SAMPLES 3840
#define SSTRIDE 16

static struct fake_sink *new_sink(void)
{
    struct fake_sink *s = calloc(1, sizeof(*s));
    s->received = [[NSMutableData alloc] init];
    s->space = SIZE_MAX;
    return s;
}

// mpv's layouts by name, as chmap.c spells them.
static void set_layout(struct mp_chmap *map, const char *name)
{
    static const struct { const char *name; int num; uint8_t speakers[8]; } named[] = {
        {"stereo", 2, {MP_SPEAKER_ID_FL, MP_SPEAKER_ID_FR}},
        {"5.1", 6, {MP_SPEAKER_ID_FL, MP_SPEAKER_ID_FR, MP_SPEAKER_ID_FC, MP_SPEAKER_ID_LFE,
                    MP_SPEAKER_ID_BL, MP_SPEAKER_ID_BR}},
        {"5.1(side)", 6, {MP_SPEAKER_ID_FL, MP_SPEAKER_ID_FR, MP_SPEAKER_ID_FC, MP_SPEAKER_ID_LFE,
                          MP_SPEAKER_ID_SL, MP_SPEAKER_ID_SR}},
        {"7.1", 8, {MP_SPEAKER_ID_FL, MP_SPEAKER_ID_FR, MP_SPEAKER_ID_FC, MP_SPEAKER_ID_LFE,
                    MP_SPEAKER_ID_BL, MP_SPEAKER_ID_BR, MP_SPEAKER_ID_SL, MP_SPEAKER_ID_SR}},
    };
    for (int n = 0; n < 4; n++) {
        if (!strcmp(named[n].name, name)) {
            map->num = named[n].num;
            memcpy(map->speaker, named[n].speakers, named[n].num);
        }
    }
}

// One player's state, which its outputs share.
static struct mpv_global player;

// TrueHD passthrough as ad_spdif sets it up: 192 kHz, 8 channels of 16 bits.
// ao_init() sets sstride only after the driver's init().
static struct ao *new_ao(struct fake_sink *s, int format)
{
    struct ao *ao = calloc(1, sizeof(*ao));
    struct priv *p = calloc(1, sizeof(*p));
    opening_sink = s;
    ao->priv = p;
    ao->global = &player;
    ao->format = format;
    ao->samplerate = 192000;
    ao->channels.num = 8;
    // As ao_alloc() does, before any init().
    atomic_store(&ao->gain, 1.0f);
    audio_out_spatial.set_gain(ao, 1.0f);
    return ao;
}

static struct mp_pcm_state state_of(struct ao *ao)
{
    struct mp_pcm_state state = {0};
    get_state(ao, &state);
    return state;
}

static int64_t now_of(struct ao *ao)
{
    return ns_from_time([((struct priv *)ao->priv)->synchronizer currentTime]);
}

// The synchronizer applies rate changes asynchronously: give the timebase a
// moment to reach the rate the driver asked for, then report what it has.
static float rate_of(struct ao *ao)
{
    struct priv *p = ao->priv;
    float asked = p->running ? 1 : 0;
    for (int n = 0; n < 200 && CMTimebaseGetRate(p->synchronizer.timebase) != asked; n++)
        usleep(1000);
    return CMTimebaseGetRate(p->synchronizer.timebase);
}

// One MAT burst lasts 20 ms.
static void write_burst(struct ao *ao, uint8_t fill)
{
    uint8_t burst[BURST_SAMPLES * SSTRIDE];
    memset(burst, fill, sizeof(burst));
    void *planes[] = {burst};
    CHECK(audio_write(ao, planes, BURST_SAMPLES), "a burst within free_samples is accepted");
}

int main(void)
{
    @autoreleasepool {
        // ao_alloc() sets the gain before init(); nothing is open to tell.
        struct fake_sink *early = new_sink();
        new_ao(early, AF_FORMAT_S_TRUEHD);
        CHECK(early->volumes == 0, "a gain set before init() reaches no sink");

        // Without a sink, or for PCM, the output refuses, so mpv uses the next one.
        struct ao *none = new_ao(NULL, AF_FORMAT_S_TRUEHD);
        CHECK(init(none) < 0, "init without a sink must fail");

        struct fake_sink *s = new_sink();
        struct ao *pcm = new_ao(s, AF_FORMAT_S16);
        CHECK(init(pcm) < 0, "init for PCM must fail");
        CHECK(s->codec == 0, "open must not be called for PCM");

        s->refuse = true;
        struct ao *refused = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(refused) < 0, "init must fail when the sink refuses");
        CHECK(s->codec == 1, "open gets the codec");
        s->refuse = false;

        struct ao *dts = new_ao(s, AF_FORMAT_S_DTSHD);
        CHECK(init(dts) == 0 && s->codec == 2, "DTS-HD opens as DTS-HD");
        uninit(dts);

        // TrueHD: opened with the synchronizer the clock runs on, at rate 0.
        struct ao *ao = new_ao(s, AF_FORMAT_S_TRUEHD);
        struct priv *p = ao->priv;
        ao->stream_silence = true;
        s->begins = 0;
        s->volumes = 0;
        CHECK(init(ao) == 0, "init for TrueHD succeeds");
        ao->sstride = SSTRIDE;
        CHECK(s->synchronizer == p->synchronizer && s->synchronizer != nil, "open gets the synchronizer");
        CHECK(s->buffer_limit == 192000 * SSTRIDE, "the sink buffers a second (%zu)", s->buffer_limit);
        CHECK(rate_of(ao) == 0, "the clock waits at rate 0");
        CHECK(s->begins == 1 && s->begin_time == now_of(ao), "init begins a timeline at the clock's time");
        CHECK(ao->device_buffer == 192000 / 2, "half a second waits in the output (%d)", ao->device_buffer);
        // Pause-as-silence: with audio-stream-silence mpv would never pause
        // this output but write zeros through the pause. Clearing the flag
        // before buffer.c reads it routes a pause to set_pause() and a seek
        // to reset(), as for any output.
        CHECK(!ao->stream_silence, "the output opts out of streamed silence");

        // mpv cannot scale a bitstream, so its volume and mute go to the sink
        // as mpv sets them (ao_set_gain() calls the driver), paused or not.
        CHECK(s->volumes == 1 && s->volume == 1.0f, "open hands the sink mpv's volume");
        set_pause(ao, true);
        audio_out_spatial.set_gain(ao, 0.0f);
        CHECK(s->volumes == 2 && s->volume == 0.0f, "a change reaches the sink at once, paused too, mute as 0");
        state_of(ao);
        CHECK(s->volumes == 2, "reading the state sends nothing more");
        set_pause(ao, false);
        audio_out_spatial.set_gain(ao, 1.0f);

        // Bursts reach the sink unchanged. What it cannot take yet waits in
        // the output, which admits only what it can hold.
        s->space = 2 * BURST_SAMPLES * SSTRIDE;
        write_burst(ao, 0x5a);
        write_burst(ao, 1);
        write_burst(ao, 2);
        CHECK(s->received.length == 2 * BURST_SAMPLES * SSTRIDE, "the sink takes what fits (%lu)",
              (unsigned long)s->received.length);
        CHECK(((const uint8_t *)s->received.bytes)[0] == 0x5a, "bytes arrive unchanged");
        struct mp_pcm_state st = state_of(ao);
        CHECK(st.free_samples == 96000 - 3840, "the rest waits and counts against free space (%d)", st.free_samples);
        s->space = SIZE_MAX;
        st = state_of(ao);
        CHECK(s->received.length == 3 * BURST_SAMPLES * SSTRIDE && st.free_samples == 96000,
              "the output offers it again when the state is read");
        CHECK(((const uint8_t *)s->received.bytes)[2 * BURST_SAMPLES * SSTRIDE] == 2, "in order");

        // Until the sink is ready, the clock holds and everything counts as queued.
        start(ao);
        for (int n = 0; n < 30; n++)
            write_burst(ao, 3);
        s->covered = s->begin_time + 100 * MS;
        st = state_of(ao);
        CHECK(rate_of(ao) == 0, "the clock waits for the sink's readiness");
        CHECK(st.playing, "reports playing while the clock waits, or mpv sees an underrun");
        CHECK(fabs(st.delay - 0.66) < 0.001, "delay is all written audio while waiting (%f)", st.delay);
        usleep(30 * 1000);
        CHECK(now_of(ao) == s->begin_time, "the clock does not move while waiting");

        // Ready but short of the lead: still waiting.
        s->ready = true;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "the first start needs the resume lead too");
        s->covered = s->begin_time + 600 * MS;
        st = state_of(ao);
        CHECK(rate_of(ao) == 1, "ready with the resume lead starts the clock");
        CHECK(fabs(st.delay - 0.66) < 0.005, "the delay still counts everything written (%f)", st.delay);

        // The clock stops short of the end of what the renderer holds, and
        // resumes once enough is queued again, on the same timeline.
        usleep(20 * 1000);
        s->covered = now_of(ao) + 200 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "the clock holds short of the end of the converted audio");
        int64_t held = now_of(ao);
        usleep(30 * 1000);
        CHECK(now_of(ao) == held, "a hold keeps the time");
        s->covered = held + 400 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "it does not resume below the resume lead");
        int begins = s->begins;
        s->covered = held + 600 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1 && s->begins == begins, "it resumes with the lead, without a new timeline");

        // A full sink means the renderer holds all it can, or conversion is
        // behind; the driver cannot tell which. Either way the clock never
        // runs past the converted audio. State reads come every 125 ms, as
        // mpv's ao thread makes them while it streams.
        s->covered = now_of(ao) + 100 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 0, "held");
        s->space = 0;
        write_burst(ao, 4);
        int changes = 0;
        bool overran = false;
        for (int n = 0; n < 6; n++) {
            float before = rate_of(ao);
            state_of(ao);
            changes += rate_of(ao) != before;
            overran |= now_of(ao) > s->covered;
            usleep(125 * 1000);
        }
        CHECK(changes == 0 && !overran && rate_of(ao) == 0,
              "a full sink below the stop lead keeps the clock held (%d rate changes)", changes);
        s->covered = now_of(ao) + 300 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1, "a full sink resumes the clock with the stop lead");
        for (int n = 0; n < 4; n++) {
            usleep(125 * 1000);
            state_of(ao);
            overran |= now_of(ao) > s->covered;
        }
        CHECK(!overran && rate_of(ao) == 0, "and holds it again before the converted audio runs out");
        s->space = SIZE_MAX;
        s->covered = now_of(ao) + 2000 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1 && !p->full, "the waiting burst goes through");

        // A pause stops the clock at once; the next state read resumes it.
        set_pause(ao, true);
        CHECK(rate_of(ao) == 0, "pause stops the clock");
        set_pause(ao, false);
        CHECK(rate_of(ao) == 0, "unpausing waits for the state read");
        state_of(ao);
        CHECK(rate_of(ao) == 1, "the state read resumes it");

        // Silence-only input, as streamed silence would be, converts to
        // nothing. The clock plays out what the renderer holds and idles
        // there, without failing, until audio arrives again.
        int64_t idle_start = now_of(ao);
        s->covered = idle_start + 600 * MS;
        for (int n = 0; n < 100; n++) {
            write_burst(ao, 0);
            state_of(ao);
            usleep(10 * 1000);
        }
        CHECK(rate_of(ao) == 0, "silence-only input holds the clock");
        int64_t idle = now_of(ao);
        CHECK(idle - idle_start > 300 * MS && idle < s->covered,
              "after playing out what was held, short of its end (%lld ms)", (long long)((idle - idle_start) / MS));
        CHECK(fallbacks == 0 && reloads == 0 && !p->reload_requested, "silence never falls back to PCM");
        s->covered = idle + 600 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1, "audio after the silence resumes the clock");

        // A gap in the passthrough stream: the queued audio is gone, and what
        // the sink had not taken yet starts a new timeline at once.
        s->space = 0;
        write_burst(ao, 5);
        s->space = SIZE_MAX;
        s->failure = MP_SPATIAL_DISCONTINUITY;
        begins = s->begins;
        int64_t gap = now_of(ao);
        st = state_of(ao);
        CHECK(s->begins == begins + 1 && s->begin_time >= gap, "a discontinuity begins a new timeline at the clock's time");
        CHECK(s->received.length == BURST_SAMPLES * SSTRIDE && ((const uint8_t *)s->received.bytes)[0] == 5,
              "the untaken burst is the new timeline's first audio");
        CHECK(rate_of(ao) == 0 && p->written == BURST_SAMPLES, "the clock waits for the new timeline (%lld)",
              (long long)p->written);
        CHECK(fabs(st.delay - 0.02) < 0.005 && fallbacks == 0, "only that burst is queued (%f)", st.delay);

        // End of stream reaches the sink after the last waiting byte.
        s->space = 0;
        write_burst(ao, 6);
        end_of_stream(ao);
        CHECK(s->ends == 0, "the end waits for the bytes before it");
        s->space = SIZE_MAX;
        st = state_of(ao);
        CHECK(s->ends == 1, "the end follows them");
        CHECK(st.playing && st.free_samples == 0, "the next file waits for this one to play out");
        state_of(ao);
        CHECK(s->ends == 1, "the end is sent once");
        // A route change flushed part of the finished stream: its tail waits
        // until the renderer holds it again, and the stream is not drained.
        s->ready = s->final = true;
        s->covered = now_of(ao) + 100 * MS;
        s->missing = 300 * MS;
        st = state_of(ao);
        CHECK(rate_of(ao) == 0 && st.playing, "a flushed tail waits for the renderer");
        // A finished stream plays out to its end, below the stop lead too.
        s->missing = 0;
        st = state_of(ao);
        CHECK(rate_of(ao) == 1 && st.playing, "the final tail plays");
        usleep(130 * 1000);
        st = state_of(ao);
        CHECK(!st.playing && st.delay == 0 && st.free_samples > 0, "a drained stream releases the next file");
        CHECK(rate_of(ao) == 0, "a drained stream stops the clock");
        begins = s->begins;
        write_burst(ao, 7);
        CHECK(s->begins == begins + 1 && !p->ended && p->playing && rate_of(ao) == 0,
              "a gapless next file begins a timeline and keeps playing");

        // A stream that ends with nothing to play drains at once.
        reset(ao);
        CHECK(s->received.length == 0 && !p->playing && rate_of(ao) == 0, "a seek discards the old carrier");
        for (int i = 0; i < 3; i++)
            write_burst(ao, 0);
        start(ao);
        end_of_stream(ao);
        CHECK(state_of(ao).playing, "the end waits for the sink");
        s->ready = s->final = true;
        s->covered = s->begin_time;
        st = state_of(ao);
        CHECK(!st.playing && st.delay == 0, "an empty end needs no audio clock");

        // 14 h of carrier stays representable.
        reset(ao);
        p->timeline = now_of(ao) - INT64_C(14) * 3600 * 1000000000;
        p->written = INT64_C(192000) * 14 * 3600 + 19200;
        CHECK(fabs(state_of(ao).delay - 0.1) < 0.001, "14 h carrier accounting remains representable");

        // A sink failure asks mpv, once, to decode the track instead, and
        // stops the clock.
        start(ao);
        s->ready = true;
        s->covered = now_of(ao) + 2000 * MS;
        state_of(ao);
        CHECK(rate_of(ao) == 1, "running before the failure");
        s->failure = MP_SPATIAL_FAILED;
        state_of(ao);
        state_of(ao);
        CHECK(fallbacks == 1 && reloads == 0 && s->failure_reads == 1,
              "a failure is logged and requests one PCM fallback (%d), not a plain reload (%d)",
              fallbacks, reloads);
        CHECK(rate_of(ao) == 0, "and stops the clock");

        uninit(ao);
        CHECK(s->closes == 2, "uninit closes the sink (%d)", s->closes);

        // The fallback belongs to the failed track, which mpv now decodes: the
        // next track or file opens a fresh output, which offers the same
        // format to the sink again.
        s->codec = 0;
        struct ao *retried = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(retried) == 0 && s->codec == 1, "the next TrueHD output asks the sink again");
        uninit(retried);
        struct ao *other = new_ao(s, AF_FORMAT_S_DTSHD);
        CHECK(init(other) == 0 && s->codec == 2, "so does the next DTS-HD output");
        uninit(other);
        struct ao *ac3 = new_ao(s, AF_FORMAT_S_AC3);
        CHECK(init(ac3) < 0, "AC-3 goes to the next output");
        CHECK(fallbacks == 1 && reloads == 0, "opening outputs after a failure requests nothing");

        // The rendered 7.1.4 as linear PCM: --ao-spatial-output. On a Mac
        // (no audio session) auto keeps APAC; iOS and tvOS take linear PCM
        // over HDMI.
        int kinds[3];
        for (int choice = SPATIAL_OUTPUT_AUTO; choice <= SPATIAL_OUTPUT_LPCM; choice++) {
            struct ao *routed = new_ao(s, AF_FORMAT_S_TRUEHD);
            ((struct priv *)routed->priv)->opt_output = choice;
            CHECK(init(routed) == 0, "TrueHD opens with output choice %d", choice);
            kinds[choice] = atomic_load(&player.spatial_output_kind);
            CHECK(s->lossless == (choice == SPATIAL_OUTPUT_LPCM), "choice %d: lossless %d", choice, s->lossless);
            uninit(routed);
            CHECK(atomic_load(&player.spatial_output_kind) == MP_SPATIAL_KIND_NONE, "closing clears the reported route");
        }
        CHECK(kinds[SPATIAL_OUTPUT_AUTO] == MP_SPATIAL_KIND_APAC && kinds[SPATIAL_OUTPUT_APAC] == MP_SPATIAL_KIND_APAC &&
              kinds[SPATIAL_OUTPUT_LPCM] == MP_SPATIAL_KIND_LPCM, "the reported route follows the choice");

        // The rendered layout: 7.1.4 unless 9.1.6 is asked for.
        struct ao *l714 = new_ao(s, AF_FORMAT_S_TRUEHD);
        CHECK(init(l714) == 0 && s->layout == 12, "7.1.4 by default (%d)", s->layout);
        uninit(l714);
        struct ao *l916 = new_ao(s, AF_FORMAT_S_TRUEHD);
        ((struct priv *)l916->priv)->opt_layout = 16;
        CHECK(init(l916) == 0 && s->layout == 16, "9.1.6 when asked for (%d)", s->layout);
        uninit(l916);

        // The renderer turning linear PCM down reopens the output, which then
        // keeps to APAC on this player; no PCM fallback.
        struct ao *lpcm = new_ao(s, AF_FORMAT_S_TRUEHD);
        struct priv *lp = lpcm->priv;
        lp->opt_output = SPATIAL_OUTPUT_LPCM;
        CHECK(init(lpcm) == 0 && s->lossless, "linear PCM asked for");
        lpcm->sstride = SSTRIDE;
        s->failure = MP_SPATIAL_OUTPUT_FAILED;
        state_of(lpcm);
        state_of(lpcm);
        CHECK(reloads == 1 && fallbacks == 1 && atomic_load(&player.spatial_lpcm_failed),
              "a refused linear PCM reloads the output once (%d), without a PCM fallback (%d)", reloads, fallbacks);
        uninit(lpcm);
        s->failure = MP_SPATIAL_OK;
        struct ao *again = new_ao(s, AF_FORMAT_S_TRUEHD);
        ((struct priv *)again->priv)->opt_output = SPATIAL_OUTPUT_LPCM;
        CHECK(init(again) == 0 && !s->lossless && atomic_load(&player.spatial_output_kind) == MP_SPATIAL_KIND_APAC,
              "the reopened output sends APAC");
        uninit(again);

        // Force Dolby MAT: off, decoded PCM is refused as before.
        struct ao *plain = new_ao(s, AF_FORMAT_FLOAT);
        plain->samplerate = 96000;
        set_layout(&plain->channels, "5.1");
        CHECK(init(plain) < 0, "without Force Dolby MAT, PCM goes to the next output");
        atomic_store(&player.spatial_lpcm_failed, false);
        // On: stereo still goes to the next output, as does AC-3.
        struct ao *stereo = new_ao(s, AF_FORMAT_FLOAT);
        ((struct priv *)stereo->priv)->opt_force_mat = true;
        set_layout(&stereo->channels, "stereo");
        CHECK(init(stereo) < 0, "stereo is never forced to MAT");
        struct ao *dd = new_ao(s, AF_FORMAT_S_AC3);
        ((struct priv *)dd->priv)->opt_force_mat = true;
        CHECK(init(dd) < 0, "AC-3 passthrough is untouched by Force Dolby MAT");
        struct ao *eac3 = new_ao(s, AF_FORMAT_S_EAC3);
        ((struct priv *)eac3->priv)->opt_force_mat = true;
        CHECK(init(eac3) < 0, "E-AC-3 passthrough is untouched by Force Dolby MAT");
        // 5.1 (back), 96 kHz S32 from a decoder: float at 48 kHz, each
        // channel in its 7.1.4 slot, surrounds at the sides.
        static const struct { const char *layout; int slots[8]; } layouts[] = {
            {"5.1", {0, 1, 2, 3, 4, 5}},
            {"5.1(side)", {0, 1, 2, 3, 4, 5}},
            {"7.1", {0, 1, 2, 3, 6, 7, 4, 5}},
        };
        for (int n = 0; n < 3; n++) {
            struct ao *mat = new_ao(s, AF_FORMAT_S32);
            struct priv *mp = mat->priv;
            mp->opt_force_mat = true;
            mat->samplerate = 96000;
            set_layout(&mat->channels, layouts[n].layout);
            s->volumes = 0;
            CHECK(init(mat) == 0, "Force Dolby MAT takes %s", layouts[n].layout);
            CHECK(mat->format == AF_FORMAT_FLOAT && mat->samplerate == 48000, "as float at 48 kHz");
            CHECK(s->codec == 0 && s->pcm_channels == mat->channels.num, "through the PCM sink (%d channels)", s->pcm_channels);
            bool placed = true;
            for (int c = 0; c < mat->channels.num; c++)
                placed &= s->slots[c] == layouts[n].slots[c];
            CHECK(placed, "%s: every channel in its 7.1.4 slot", layouts[n].layout);
            CHECK(s->buffer_limit == 48000 * 4 * (size_t)mat->channels.num, "a second of PCM buffered (%zu)", s->buffer_limit);
            CHECK(atomic_load(&player.spatial_output_kind) == MP_SPATIAL_KIND_PCM, "reported as PCM to MAT");
            CHECK(s->volumes == 1 && s->volume == 1.0f, "the renderer plays at unity");
            audio_out_spatial.set_gain(mat, 0.25f);
            CHECK(s->volumes == 1, "mpv's volume is mpv's own on decoded PCM, never applied twice");
            CHECK(offered_speakers == ((UINT64_C(1) << MP_SPEAKER_ID_FL) | (UINT64_C(1) << MP_SPEAKER_ID_FR) |
                  (UINT64_C(1) << MP_SPEAKER_ID_FC) | (UINT64_C(1) << MP_SPEAKER_ID_LFE) |
                  (UINT64_C(1) << MP_SPEAKER_ID_SL) | (UINT64_C(1) << MP_SPEAKER_ID_SR) |
                  (UINT64_C(1) << MP_SPEAKER_ID_BL) | (UINT64_C(1) << MP_SPEAKER_ID_BR) |
                  (UINT64_C(1) << MP_SPEAKER_ID_TFL) | (UINT64_C(1) << MP_SPEAKER_ID_TFR) |
                  (UINT64_C(1) << MP_SPEAKER_ID_TBL) | (UINT64_C(1) << MP_SPEAKER_ID_TBR)),
                  "only the 7.1.4 speakers are offered");
            uninit(mat);
        }
        // A renderer refusing the PCM leaves it to the next output.
        struct ao *refusing = new_ao(s, AF_FORMAT_FLOAT);
        ((struct priv *)refusing->priv)->opt_force_mat = true;
        set_layout(&refusing->channels, "5.1");
        CHECK(init(refusing) == 0, "opens");
        refusing->sstride = 24;
        s->failure = MP_SPATIAL_OUTPUT_FAILED;
        int before = reloads;
        state_of(refusing);
        CHECK(reloads == before + 1 && fallbacks == 1, "a refusal reloads the output, no track fallback");
        uninit(refusing);
        s->failure = MP_SPATIAL_OK;
        struct ao *after = new_ao(s, AF_FORMAT_FLOAT);
        ((struct priv *)after->priv)->opt_force_mat = true;
        set_layout(&after->channels, "5.1");
        CHECK(init(after) < 0, "and the reopened chain plays it on the usual output");
    }
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
