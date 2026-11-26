// Dolby Vision RPUs through the Apple series, on every RPU in libdovi's test
// assets (test_dovi_rpu_layers.sh builds the host libavcodec this links):
//
//  1. mpv 0316's enhancement layer kind (the "dovi el kind" region of
//     video/mp_image.c, compiled verbatim as dovi_el_kind.inc) of the RPU
//     FFmpeg parses agrees with dovi_tool's own summary for that file:
//     "Profile: 7 (FEL)" fel, "(MEL)" mel, profiles 5 and 8 none.
//  2. FFmpeg 0304's ff_dovi_attach_rpu_buffer() writes the parsed RPU back in
//     the HEVC layout (0x19 prefix, emulation prevention) and FFmpeg parses
//     that to the same header, mapping, colour metadata and extension blocks.
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/dovi_meta.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavcodec/dovi_rpu.h"

#include "dovi_el_kind.inc"

static int failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
    } while (0)

// dovi_tool 2.3.4's `info --summary` for each asset (libdovi-3.3.2's
// assets/tests); profile 4 and the asset dovi_tool cannot parse are left out.
static const struct { const char *name; int kind; } expected[] = {
    {"cmv2_9_xml_with_l5_rpu.bin", 0}, {"cmv40_full_rpu.bin", 0},
    {"cmv4_0_2_custom_displays_xml_rpu.bin", 0}, {"cmv4_0_2_xml_rpu.bin", 0},
    {"cmv4_0_2_xml_with_l5_rpu.bin", 0}, {"cmv4_2_510_xml_rpu.bin", 0},
    {"data_before_crc32.bin", 2}, {"empty_dmv1_blocks.bin", 0},
    {"eof_rpu.bin", 2}, {"fel_orig.bin", 2}, {"fel_rpu.bin", 2},
    {"fel_to_81.bin", 0}, {"fel_to_mel.bin", 1}, {"fix_se_write.bin", 2},
    {"mel_orig.bin", 1}, {"mel_rpu.bin", 1}, {"mel_to_81.bin", 0},
    {"mel_to_mel.bin", 1}, {"mel_variable_l8_length13.bin", 1},
    {"p8_001_end_crc32.bin", 0}, {"poly_coef_int_logic.bin", 2},
    {"profile20_apple.bin", 0}, {"profile5-02.bin", 0}, {"profile5.bin", 0},
    {"profile8.bin", 0}, {"profile84.bin", 0},
    {"profile8_from_profile5-02.bin", 0},
    {"source_p5_to_p8_001_end_crc32.bin", 0},
    {"source_rpu_replaced_fel_orig.bin", 2}, {"trailing_bytes_rpu.bin", 2},
    {"unordered_l8_blocks.bin", 0},
};

// RPUs FFmpeg's writer does not reproduce, so ff_dovi_attach_rpu_buffer()
// must refuse them (its verifying parse) and attach nothing: an empty
// DM v1 extension block list.
static const char *unwritable[] = {"empty_dmv1_blocks.bin"};
// Written and parsed, with colour metadata the writer does not reproduce:
// profile 4, which no AV1 stream carries.
static const char *colour_differs[] = {"profile4.bin"};

static int listed(const char **list, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(list[i], name))
            return 1;
    return 0;
}

static const char *kind_name(int kind)
{
    return kind == 2 ? "fel" : kind == 1 ? "mel" : "none";
}

// Emulation prevention removed, as the HEVC decoder hands ff_dovi_rpu_parse().
static size_t unescape(const uint8_t *in, size_t size, uint8_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        if (i >= 2 && in[i] == 3 && !in[i - 1] && !in[i - 2])
            continue;
        out[n++] = in[i];
    }
    return n;
}

static AVDOVIMetadata *parse(const uint8_t *escaped, size_t size)
{
    DOVIContext ctx = {0};
    AVDOVIMetadata *meta = NULL;
    uint8_t *raw = av_malloc(size + 64);
    size_t n = unescape(escaped, size, raw);
    memset(raw + n, 0, 64);
    if (ff_dovi_rpu_parse(&ctx, raw, n, 0) >= 0 && ff_dovi_get_metadata(&ctx, &meta) <= 0)
        meta = NULL;
    ff_dovi_ctx_unref(&ctx);
    av_free(raw);
    return meta;
}

static int same_blocks(const AVDOVIMetadata *a, const AVDOVIMetadata *b)
{
    if (a->num_ext_blocks != b->num_ext_blocks)
        return 0;
    // The writer orders static blocks after dynamic ones: compare as sets.
    for (int i = 0; i < a->num_ext_blocks; i++) {
        const AVDOVIDmData *x = av_dovi_get_ext(a, i);
        int found = 0;
        for (int j = 0; j < b->num_ext_blocks && !found; j++)
            found = !memcmp(x, av_dovi_get_ext(b, j), a->ext_block_size);
        if (!found)
            return 0;
    }
    return 1;
}

static void check_file(const char *path)
{
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size);
    if (fread(data, 1, size, f) != (size_t)size)
        size = 0;
    fclose(f);

    int want = -1;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        if (!strcmp(expected[i].name, name))
            want = expected[i].kind;

    // The file is RPUs after 4-byte start codes; the first is checked.
    long start = 4, end = size;
    for (long i = start; i + 4 <= size; i++) {
        if (!data[i] && !data[i + 1] && !data[i + 2] && data[i + 3] == 1) {
            end = i;
            break;
        }
    }
    AVDOVIMetadata *meta = size > 4 ? parse(data + start, end - start) : NULL;
    if (!meta) {
        printf("skip %s: FFmpeg does not parse it\n", name);
        free(data);
        return;
    }

    int kind = mp_dovi_el_from_av(meta);
    if (want >= 0)
        CHECK(kind == want, "%s: %s, dovi_tool says %s", name, kind_name(kind), kind_name(want));

    // Round trip through ff_dovi_attach_rpu_buffer().
    DOVIContext dec = {0}, enc = {0};
    dec.cfg.dv_profile = 8;
    AVFrame *frame = av_frame_alloc();
    size_t meta_size = meta->ext_block_offset + meta->num_ext_blocks * meta->ext_block_size;
    AVFrameSideData *sd = av_frame_new_side_data(frame, AV_FRAME_DATA_DOVI_METADATA, meta_size);
    memcpy(sd->data, meta, meta_size);
    int ret = ff_dovi_attach_rpu_buffer(&enc, &dec, frame);
    const AVFrameSideData *rpu = av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (listed(unwritable, sizeof(unwritable) / sizeof(*unwritable), name)) {
        CHECK(ret < 0 && !rpu, "%s: an RPU FFmpeg cannot write back was attached", name);
    } else {
        CHECK(ret >= 0 && rpu && rpu->size > 1 && rpu->data[0] == 0x19,
              "%s: no RPU buffer written (%d)", name, ret);
    }
    if (ret >= 0 && rpu && rpu->size > 1) {
        AVDOVIMetadata *back = parse(rpu->data, rpu->size);
        CHECK(back, "%s: the written RPU does not parse", name);
        if (back) {
            CHECK(!memcmp(av_dovi_get_header(meta), av_dovi_get_header(back), sizeof(AVDOVIRpuDataHeader)),
                  "%s: header differs after the round trip", name);
            CHECK(!memcmp(av_dovi_get_mapping(meta), av_dovi_get_mapping(back), sizeof(AVDOVIDataMapping)),
                  "%s: mapping differs after the round trip", name);
            CHECK(listed(colour_differs, sizeof(colour_differs) / sizeof(*colour_differs), name) ||
                  !memcmp(av_dovi_get_color(meta), av_dovi_get_color(back), sizeof(AVDOVIColorMetadata)),
                  "%s: colour metadata differs after the round trip", name);
            CHECK(same_blocks(meta, back), "%s: extension blocks differ after the round trip", name);
            CHECK(mp_dovi_el_from_av(back) == kind, "%s: kind changed after the round trip", name);
            av_free(back);
        }
    }
    // A frame that already has one keeps it.
    if (rpu) {
        const uint8_t *before = rpu->data;
        ff_dovi_attach_rpu_buffer(&enc, &dec, frame);
        CHECK(av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER)->data == before,
              "%s: a second call replaced the RPU buffer", name);
    }
    printf("%-40s %s%s\n", name, kind_name(kind), want < 0 ? " (no expectation)" : "");
    av_frame_free(&frame);
    ff_dovi_ctx_unref(&enc);
    av_free(meta);
    free(data);
}

int main(int argc, char **argv)
{
    // The decision alone: identity NLQ in fixed point at two precisions.
    const uint16_t zero16[3] = {0};
    const uint64_t zero[3] = {0};
    const uint64_t one23[3] = {1ull << 23, 1ull << 23, 1ull << 23};
    const uint64_t one10[3] = {1ull << 10, 1ull << 10, 1ull << 10};
    const uint64_t off[3] = {1ull << 23, (1ull << 23) + 1, 1ull << 23};
    const uint16_t offset[3] = {0, 0, 512};
    CHECK(mp_dovi_el_kind(0, 0, 0, 23, zero16, one23, zero, zero) == 1, "identity NLQ is MEL");
    CHECK(mp_dovi_el_kind(0, 0, 0, 10, zero16, one10, zero, zero) == 1, "identity NLQ at 10 bits is MEL");
    CHECK(mp_dovi_el_kind(0, 0, 0, 23, zero16, off, zero, zero) == 2, "vdr_in_max off 1.0 is FEL");
    CHECK(mp_dovi_el_kind(0, 0, 0, 23, offset, one23, zero, zero) == 2, "an NLQ offset is FEL");
    CHECK(mp_dovi_el_kind(0, 0, 0, 23, zero16, one23, one10, zero) == 2, "a dead zone slope is FEL");
    CHECK(mp_dovi_el_kind(1, 0, 0, 23, offset, off, zero, zero) == 0, "no residual is none");
    CHECK(mp_dovi_el_kind(0, 0x100, 0, 23, offset, off, zero, zero) == 0, "another layout is none");
    CHECK(mp_dovi_el_kind(0, 0, -1, 23, offset, off, zero, zero) == 0, "no NLQ method is none");
    CHECK(mp_dovi_el_kind(0, 0, 0, 64, zero16, one23, zero, zero) == 0, "an impossible precision is none");

    for (int i = 1; i < argc; i++)
        check_file(argv[i]);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dovi rpu layers: all checks passed (%d files)\n", argc - 1);
    return 0;
}
