/*
 * mbvenc - encoder for MBV ("Marty Block Video"), the codec described in
 * video.txt and specified in docs/MBV_FORMAT.md.
 *
 *   mbvenc [options] video.rgb24 audio.u8 out.mbv
 *
 * Input is raw RGB24 frames at the target resolution and raw unsigned 8-bit
 * mono audio, both of which ffmpeg produces directly; tools/mkmbv.sh does that
 * part.  Output is a single interleaved MBV stream ready to be streamed off
 * the CD a chunk at a time.
 *
 * Two things are worth knowing about how this works.
 *
 * Everything is measured in palette-index space.  Each GOP gets its own
 * 256-colour palette (median cut over that GOP's frames), the frames are
 * quantised to it once, and from then on both the target picture and the
 * reconstruction are arrays of palette indices.  All error arithmetic is then
 * a lookup in a 256x256 table of squared RGB distances between palette
 * entries, which is what makes an exhaustive-ish search affordable and keeps
 * the encoder's notion of "error" identical to what the machine will show.
 *
 * The reconstruction is built by calling the decoder's own primitives
 * (fmt_mbv_* in src/common/mbv.c) in stream order.  MBV decodes in place, so a
 * motion vector can source pixels this frame has already rewritten; rather
 * than forbid that, the encoder simply performs the same in-place copy the
 * decoder will and measures the actual result.  Encoder and decoder therefore
 * cannot disagree - tests/mbv_roundtrip_test.c re-decodes the encoder's output
 * and checks it byte for byte.
 *
 * Block choice is rate-distortion: each candidate coding of a block costs
 * err + lambda*bytes, and the cheapest wins.  Lambda is steered by a feedback
 * loop towards a byte budget per frame, because the real constraint here is
 * not quality, it is that a 1x CD delivers ~150KB/s and the 386SX has to spend
 * most of its time doing other things.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mbv.h"

#define MAX_COST        0x3fffffffu
#define PAL_SIZE        256

/* ------------------------------------------------------------------ */
/* Options                                                            */
/* ------------------------------------------------------------------ */

static int   opt_w = 256, opt_h = 240;
static double opt_fps = 12.0;
static int   opt_audio_rate = 16000;
static int   opt_gop = 24;
static int   opt_budget = 5000;       /* bytes/frame for inter frames */
static int   opt_key_budget = 20000;  /* bytes/frame for keyframes */
static int   opt_max_chunk = 30000;   /* hard ceiling; must fit the player's scratch */
static int   opt_search = 8;          /* motion search radius, in pixels */
static int   opt_quiet = 0;
static const char *opt_dump = NULL;    /* reconstruction dump, for the tests */

/* ------------------------------------------------------------------ */
/* Frame state                                                        */
/* ------------------------------------------------------------------ */

static int W, H, MBW, MBH;
static uint8_t *g_rgb;        /* one GOP of source frames, RGB24 */
static uint8_t *g_target;     /* current frame quantised to the palette */
static uint8_t *g_recon;      /* the reconstruction, decoder-identical */
static uint8_t *g_recon_save; /* copy taken at frame start, for retries */
static uint8_t  g_pal[768];
static uint32_t (*g_dist)[PAL_SIZE];   /* squared RGB distance between entries */
static uint8_t *g_map15;      /* RGB555 -> nearest palette index */

static void die(const char *msg)
{
    fprintf(stderr, "mbvenc: %s\n", msg);
    exit(1);
}

static void *xalloc(size_t n)
{
    void *p = calloc(1, n);
    if (!p) {
        die("out of memory");
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Palette generation: median cut over a 5-bit histogram               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t count;
    uint32_t sr, sg, sb;     /* 8-bit component sums */
    uint8_t  r, g, b;        /* 5-bit cell coordinates */
} cell_t;

typedef struct {
    int start, end;          /* half-open range in the cell array */
    uint32_t count;
    uint8_t rmin, rmax, gmin, gmax, bmin, bmax;
} box_t;

static int g_sort_axis;

static int cell_cmp(const void *a, const void *b)
{
    const cell_t *ca = a, *cb = b;
    int va, vb;

    switch (g_sort_axis) {
    case 0:  va = ca->r; vb = cb->r; break;
    case 1:  va = ca->g; vb = cb->g; break;
    default: va = ca->b; vb = cb->b; break;
    }
    return va - vb;
}

static void box_bounds(box_t *bx, const cell_t *cells)
{
    int i;

    bx->rmin = bx->gmin = bx->bmin = 31;
    bx->rmax = bx->gmax = bx->bmax = 0;
    bx->count = 0;
    for (i = bx->start; i < bx->end; i++) {
        const cell_t *c = &cells[i];
        if (c->r < bx->rmin) bx->rmin = c->r;
        if (c->r > bx->rmax) bx->rmax = c->r;
        if (c->g < bx->gmin) bx->gmin = c->g;
        if (c->g > bx->gmax) bx->gmax = c->g;
        if (c->b < bx->bmin) bx->bmin = c->b;
        if (c->b > bx->bmax) bx->bmax = c->b;
        bx->count += c->count;
    }
}

/*
 * Builds a 256-entry palette for `nframes` RGB24 frames.
 *
 * Median cut rather than anything fancier because it is stable: neighbouring
 * GOPs of similar material get similar palettes, which matters here in a way
 * it would not for still images.  The palette only changes at keyframes, and a
 * keyframe is a full refresh, so a wildly different palette every two seconds
 * would read as a visible pulse even when the picture barely moved.
 */
static void build_palette(const uint8_t *frames, int nframes)
{
    static cell_t cells[32768];
    box_t boxes[PAL_SIZE];
    int ncells = 0, nboxes, i;
    size_t px = (size_t)W * H * nframes;
    const uint8_t *p = frames;

    memset(cells, 0, sizeof(cells));
    for (i = 0; i < 32768; i++) {
        cells[i].r = (uint8_t)(i >> 10);
        cells[i].g = (uint8_t)((i >> 5) & 31);
        cells[i].b = (uint8_t)(i & 31);
    }
    while (px--) {
        int idx = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
        cells[idx].count++;
        cells[idx].sr += p[0];
        cells[idx].sg += p[1];
        cells[idx].sb += p[2];
        p += 3;
    }
    for (i = 0; i < 32768; i++) {
        if (cells[i].count) {
            cells[ncells++] = cells[i];
        }
    }
    if (ncells == 0) {
        memset(g_pal, 0, sizeof(g_pal));
        return;
    }

    boxes[0].start = 0;
    boxes[0].end = ncells;
    box_bounds(&boxes[0], cells);
    nboxes = 1;

    while (nboxes < PAL_SIZE) {
        int best = -1, axis, split, j;
        uint32_t best_count = 0, half, acc;
        box_t *bx;

        /* Split the most populated box that still has room to be split. */
        for (i = 0; i < nboxes; i++) {
            if (boxes[i].end - boxes[i].start < 2) {
                continue;
            }
            if (boxes[i].count > best_count) {
                best_count = boxes[i].count;
                best = i;
            }
        }
        if (best < 0) {
            break;   /* fewer distinct colours than palette entries */
        }
        bx = &boxes[best];

        axis = 0;
        if (bx->gmax - bx->gmin > bx->rmax - bx->rmin) {
            axis = 1;
        }
        if (bx->bmax - bx->bmin >
            (axis == 0 ? bx->rmax - bx->rmin : bx->gmax - bx->gmin)) {
            axis = 2;
        }
        g_sort_axis = axis;
        qsort(cells + bx->start, (size_t)(bx->end - bx->start),
              sizeof(cell_t), cell_cmp);

        half = bx->count / 2;
        acc = 0;
        split = bx->start;
        for (j = bx->start; j < bx->end - 1; j++) {
            acc += cells[j].count;
            split = j + 1;
            if (acc >= half) {
                break;
            }
        }

        boxes[nboxes].start = split;
        boxes[nboxes].end = bx->end;
        bx->end = split;
        box_bounds(bx, cells);
        box_bounds(&boxes[nboxes], cells);
        nboxes++;
    }

    memset(g_pal, 0, sizeof(g_pal));
    for (i = 0; i < nboxes; i++) {
        uint32_t sr = 0, sg = 0, sb = 0, n = 0;
        int j;
        for (j = boxes[i].start; j < boxes[i].end; j++) {
            sr += cells[j].sr;
            sg += cells[j].sg;
            sb += cells[j].sb;
            n  += cells[j].count;
        }
        if (!n) {
            continue;
        }
        g_pal[i * 3 + 0] = (uint8_t)(sr / n);
        g_pal[i * 3 + 1] = (uint8_t)(sg / n);
        g_pal[i * 3 + 2] = (uint8_t)(sb / n);
    }
}

static void build_tables(void)
{
    int i, j;

    for (i = 0; i < PAL_SIZE; i++) {
        for (j = 0; j < PAL_SIZE; j++) {
            int dr = g_pal[i * 3 + 0] - g_pal[j * 3 + 0];
            int dg = g_pal[i * 3 + 1] - g_pal[j * 3 + 1];
            int db = g_pal[i * 3 + 2] - g_pal[j * 3 + 2];
            g_dist[i][j] = (uint32_t)(dr * dr + dg * dg + db * db);
        }
    }

    /* Nearest palette entry for every 5-bit-per-channel colour.  Quantising
     * through this costs one shift-and-mask per pixel instead of a 256-entry
     * search, and the palette came out of a 5-bit histogram anyway. */
    for (i = 0; i < 32768; i++) {
        int r = ((i >> 10) << 3) | 4;
        int g = (((i >> 5) & 31) << 3) | 4;
        int b = ((i & 31) << 3) | 4;
        uint32_t best = 0xffffffffu;
        int bestj = 0;

        for (j = 0; j < PAL_SIZE; j++) {
            int dr = r - g_pal[j * 3 + 0];
            int dg = g - g_pal[j * 3 + 1];
            int db = b - g_pal[j * 3 + 2];
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < best) {
                best = d;
                bestj = j;
            }
        }
        g_map15[i] = (uint8_t)bestj;
    }
}

static void quantise_frame(const uint8_t *rgb, uint8_t *out)
{
    size_t n = (size_t)W * H;

    while (n--) {
        out[0] = g_map15[((rgb[0] >> 3) << 10) | ((rgb[1] >> 3) << 5) | (rgb[2] >> 3)];
        out++;
        rgb += 3;
    }
}

/* ------------------------------------------------------------------ */
/* Error measurement                                                   */
/* ------------------------------------------------------------------ */

static uint32_t region_err(int x, int y, int n)
{
    uint32_t e = 0;
    int i, j;

    for (i = 0; i < n; i++) {
        const uint8_t *r = g_recon + (size_t)(y + i) * W + x;
        const uint8_t *t = g_target + (size_t)(y + i) * W + x;
        for (j = 0; j < n; j++) {
            e += g_dist[r[j]][t[j]];
        }
    }
    return e;
}

static void region_save(int x, int y, int n, uint8_t *save)
{
    int i;
    for (i = 0; i < n; i++) {
        memcpy(save + i * n, g_recon + (size_t)(y + i) * W + x, (size_t)n);
    }
}

static void region_restore(int x, int y, int n, const uint8_t *save)
{
    int i;
    for (i = 0; i < n; i++) {
        memcpy(g_recon + (size_t)(y + i) * W + x, save + i * n, (size_t)n);
    }
}

/* ------------------------------------------------------------------ */
/* Colour selection for the 4x4 block types                            */
/* ------------------------------------------------------------------ */

/*
 * Picks `n` palette entries to represent 16 target pixels and assigns each
 * pixel to one of them, returning the total error.
 *
 * The candidate entries are only ever colours that actually occur in the
 * block: with the frame already quantised, anything else would have to be a
 * better representative of these pixels than the entries chosen for them, and
 * restricting the search this way turns the clustering into a handful of table
 * lookups.  Exact whenever the block has at most n distinct colours, which for
 * CLR4/CLR8 is most of them.
 */
static uint32_t pick_colors(const uint8_t *t, int n, uint8_t *colors, uint8_t *assign)
{
    uint8_t distinct[16];
    int nd = 0, i, j, iter;
    uint32_t err;

    for (i = 0; i < 16; i++) {
        for (j = 0; j < nd; j++) {
            if (distinct[j] == t[i]) {
                break;
            }
        }
        if (j == nd) {
            distinct[nd++] = t[i];
        }
    }

    if (nd <= n) {
        for (i = 0; i < n; i++) {
            colors[i] = distinct[i < nd ? i : 0];
        }
        for (i = 0; i < 16; i++) {
            for (j = 0; j < nd; j++) {
                if (distinct[j] == t[i]) {
                    assign[i] = (uint8_t)j;
                    break;
                }
            }
        }
        return 0;
    }

    /* Farthest-point seeding, then Lloyd iterations whose "centroid" is the
     * cluster member minimising the distance sum - still a palette index. */
    colors[0] = distinct[0];
    for (i = 1; i < n; i++) {
        uint32_t far = 0;
        int pick = 0;
        for (j = 0; j < nd; j++) {
            uint32_t nearest = 0xffffffffu;
            int k;
            for (k = 0; k < i; k++) {
                uint32_t d = g_dist[colors[k]][distinct[j]];
                if (d < nearest) {
                    nearest = d;
                }
            }
            if (nearest > far) {
                far = nearest;
                pick = j;
            }
        }
        colors[i] = distinct[pick];
    }

    for (iter = 0; iter < 4; iter++) {
        uint32_t best_sum[8];
        uint8_t best_c[8];
        int changed = 0;

        err = 0;
        for (i = 0; i < 16; i++) {
            uint32_t best = 0xffffffffu;
            int bj = 0;
            for (j = 0; j < n; j++) {
                uint32_t d = g_dist[colors[j]][t[i]];
                if (d < best) {
                    best = d;
                    bj = j;
                }
            }
            assign[i] = (uint8_t)bj;
            err += best;
        }
        if (iter == 3) {
            break;
        }

        for (j = 0; j < n; j++) {
            best_sum[j] = 0xffffffffu;
            best_c[j] = colors[j];
        }
        for (j = 0; j < n; j++) {
            int c;
            for (c = 0; c < nd; c++) {
                uint32_t sum = 0;
                for (i = 0; i < 16; i++) {
                    if (assign[i] == j) {
                        sum += g_dist[distinct[c]][t[i]];
                    }
                }
                if (sum < best_sum[j]) {
                    best_sum[j] = sum;
                    best_c[j] = distinct[c];
                }
            }
        }
        for (j = 0; j < n; j++) {
            if (best_sum[j] != 0xffffffffu && best_c[j] != colors[j]) {
                colors[j] = best_c[j];
                changed = 1;
            }
        }
        if (!changed) {
            break;
        }
    }
    return err;
}

/* Best single palette index for an n x n region of the target. */
static uint32_t pick_fill(int x, int y, int n, uint8_t *color)
{
    uint8_t distinct[64];
    int nd = 0, i, j, k;
    uint32_t best = MAX_COST;

    for (i = 0; i < n; i++) {
        const uint8_t *t = g_target + (size_t)(y + i) * W + x;
        for (j = 0; j < n; j++) {
            for (k = 0; k < nd; k++) {
                if (distinct[k] == t[j]) {
                    break;
                }
            }
            if (k == nd) {
                distinct[nd++] = t[j];
            }
        }
    }

    *color = distinct[0];
    for (k = 0; k < nd; k++) {
        uint32_t sum = 0;
        for (i = 0; i < n; i++) {
            const uint8_t *t = g_target + (size_t)(y + i) * W + x;
            for (j = 0; j < n; j++) {
                sum += g_dist[distinct[k]][t[j]];
            }
        }
        if (sum < best) {
            best = sum;
            *color = distinct[k];
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Motion search                                                       */
/* ------------------------------------------------------------------ */

/*
 * Evaluates one vector by actually performing the copy the decoder would, on
 * the real reconstruction, then undoing it.  MBV motion sources from the frame
 * being written, so for any vector shorter than the block itself the result
 * depends on the copy's own overlap; simulating it any other way would be
 * simulating a different codec.
 */
static uint32_t try_mv(int x, int y, int n, int dx, int dy, const uint8_t *save)
{
    uint32_t e;

    if (x + dx < 0 || x + dx + n > W || y + dy < 0 || y + dy + n > H) {
        return MAX_COST;
    }
    fmt_mbv_motion(g_recon, W, x, y, dx, dy, n);
    e = region_err(x, y, n);
    region_restore(x, y, n, save);
    return e;
}

static uint32_t motion_search(int x, int y, int n, const uint8_t *save,
                             int seed_dx, int seed_dy, int *out_dx, int *out_dy)
{
    static const int dirs[9][2] = {
        { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
        { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
    };
    int bx = 0, by = 0, step;
    uint32_t best;

    best = try_mv(x, y, n, 0, 0, save);
    if (seed_dx || seed_dy) {
        uint32_t e = try_mv(x, y, n, seed_dx, seed_dy, save);
        if (e < best) {
            best = e;
            bx = seed_dx;
            by = seed_dy;
        }
    }

    /* Three-step search from whichever of those two was better. */
    for (step = opt_search >= 8 ? 4 : 2; step >= 1; step >>= 1) {
        int improved = 1;
        while (improved) {
            int i, nx = bx, ny = by;
            improved = 0;
            for (i = 1; i < 9; i++) {
                int cx = bx + dirs[i][0] * step;
                int cy = by + dirs[i][1] * step;
                uint32_t e;
                if (cx < MBV_MV_MIN || cx > MBV_MV_MAX ||
                    cy < MBV_MV_MIN || cy > MBV_MV_MAX ||
                    cx < -opt_search || cx > opt_search ||
                    cy < -opt_search || cy > opt_search) {
                    continue;
                }
                e = try_mv(x, y, n, cx, cy, save);
                if (e < best) {
                    best = e;
                    nx = cx;
                    ny = cy;
                    improved = 1;
                }
            }
            bx = nx;
            by = ny;
            if (step > 1) {
                break;   /* coarse steps are single-pass */
            }
        }
    }

    *out_dx = bx;
    *out_dy = by;
    return best;
}

/* ------------------------------------------------------------------ */
/* Block and macroblock coding                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t buf[4096];
    int len;
} obuf;

static void put8(obuf *o, unsigned v)
{
    if (o->len >= (int)sizeof(o->buf)) {
        die("internal buffer overflow");
    }
    o->buf[o->len++] = (uint8_t)v;
}

/*
 * Codes one 4x4 block, applying the winner to the reconstruction and
 * appending it to `o`.  Applying immediately is required, not merely
 * convenient: the next block's SKIP and MOTION options are evaluated against
 * the frame as it now stands, which is what the decoder will have too.
 */
static uint32_t code_block(int x, int y, uint32_t lambda, int keyframe,
                           int seed_dx, int seed_dy, obuf *o)
{
    uint8_t save[16], t[16];
    uint8_t colors[8], assign[16];
    uint8_t c2[2], c4[4], c8[8], a2[16], a4[16], a8[16];
    uint32_t cost[MBV_B_COUNT], best_cost;
    uint32_t e2, e4, e8;
    uint8_t fill_color = 0;
    int mvx = 0, mvy = 0, best_op, i;

    region_save(x, y, 4, save);
    for (i = 0; i < 4; i++) {
        memcpy(t + i * 4, g_target + (size_t)(y + i) * W + x, 4);
    }

    for (i = 0; i < (int)MBV_B_COUNT; i++) {
        cost[i] = MAX_COST;
    }

    if (!keyframe) {
        uint32_t e = region_err(x, y, 4);
        cost[MBV_B_SKIP] = e + lambda * MBV_B_BYTES(MBV_B_SKIP);
        if (e == 0) {
            /* Already exact, and every other coding costs at least two
             * bytes, so nothing can beat this. */
            put8(o, MBV_B_SKIP);
            return cost[MBV_B_SKIP];
        }
        cost[MBV_B_MOTION] = motion_search(x, y, 4, save, seed_dx, seed_dy,
                                           &mvx, &mvy);
        if (cost[MBV_B_MOTION] != MAX_COST) {
            cost[MBV_B_MOTION] += lambda * MBV_B_BYTES(MBV_B_MOTION);
        }
    }

    cost[MBV_B_FILL] = pick_fill(x, y, 4, &fill_color)
                     + lambda * MBV_B_BYTES(MBV_B_FILL);

    e2 = pick_colors(t, 2, colors, assign);
    memcpy(c2, colors, 2);
    memcpy(a2, assign, 16);
    cost[MBV_B_CLR2] = e2 + lambda * MBV_B_BYTES(MBV_B_CLR2);

    if (e2) {
        e4 = pick_colors(t, 4, colors, assign);
        memcpy(c4, colors, 4);
        memcpy(a4, assign, 16);
        cost[MBV_B_CLR4] = e4 + lambda * MBV_B_BYTES(MBV_B_CLR4);

        if (e4) {
            e8 = pick_colors(t, 8, colors, assign);
            memcpy(c8, colors, 8);
            memcpy(a8, assign, 16);
            cost[MBV_B_CLR8] = e8 + lambda * MBV_B_BYTES(MBV_B_CLR8);
            if (e8) {
                cost[MBV_B_RAW] = lambda * MBV_B_BYTES(MBV_B_RAW);
            }
        }
    }

    best_op = MBV_B_RAW;
    best_cost = cost[MBV_B_RAW];
    for (i = 0; i < (int)MBV_B_COUNT; i++) {
        if (cost[i] < best_cost) {
            best_cost = cost[i];
            best_op = i;
        }
    }

    put8(o, (unsigned)best_op);
    switch (best_op) {
    case MBV_B_SKIP:
        break;
    case MBV_B_MOTION:
        put8(o, MBV_MV_PACK(mvx, mvy));
        fmt_mbv_motion(g_recon, W, x, y, mvx, mvy, 4);
        break;
    case MBV_B_FILL:
        put8(o, fill_color);
        fmt_mbv_fill(g_recon + (size_t)y * W + x, W, 4, fill_color);
        break;
    case MBV_B_CLR2: {
        uint16_t map = 0;
        for (i = 0; i < 16; i++) {
            map |= (uint16_t)(a2[i] & 1) << i;
        }
        put8(o, c2[0]);
        put8(o, c2[1]);
        put8(o, map & 0xff);
        put8(o, map >> 8);
        fmt_mbv_clr2(g_recon + (size_t)y * W + x, W, c2, map);
        break;
    }
    case MBV_B_CLR4: {
        uint8_t map[4];
        for (i = 0; i < 4; i++) {
            map[i] = (uint8_t)(a4[i * 4 + 0] | (a4[i * 4 + 1] << 2) |
                               (a4[i * 4 + 2] << 4) | (a4[i * 4 + 3] << 6));
        }
        for (i = 0; i < 4; i++) {
            put8(o, c4[i]);
        }
        for (i = 0; i < 4; i++) {
            put8(o, map[i]);
        }
        fmt_mbv_clr4(g_recon + (size_t)y * W + x, W, c4, map);
        break;
    }
    case MBV_B_CLR8: {
        uint8_t map[6];
        int half;
        for (half = 0; half < 2; half++) {
            uint32_t acc = 0;
            for (i = 0; i < 8; i++) {
                acc |= (uint32_t)(a8[half * 8 + i] & 7) << (i * 3);
            }
            map[half * 3 + 0] = (uint8_t)(acc & 0xff);
            map[half * 3 + 1] = (uint8_t)((acc >> 8) & 0xff);
            map[half * 3 + 2] = (uint8_t)((acc >> 16) & 0xff);
        }
        for (i = 0; i < 8; i++) {
            put8(o, c8[i]);
        }
        for (i = 0; i < 6; i++) {
            put8(o, map[i]);
        }
        fmt_mbv_clr8(g_recon + (size_t)y * W + x, W, c8, map);
        break;
    }
    case MBV_B_RAW:
        for (i = 0; i < 16; i++) {
            put8(o, t[i]);
        }
        fmt_mbv_raw(g_recon + (size_t)y * W + x, W, 4, t);
        break;
    }
    return best_cost;
}

typedef struct {
    int dx, dy;
} mv_t;

/*
 * Codes one 8x8 macroblock.  Returns the opcode chosen; `o` receives the
 * macroblock's bytes (SKIP writes nothing - the caller merges those into a
 * run) and the reconstruction is left holding the result.
 */
static int code_macroblock(int mbx, int mby, uint32_t lambda, int keyframe,
                           mv_t *pred, obuf *o)
{
    uint8_t save[64];
    obuf split;
    uint32_t cost_skip = MAX_COST, cost_mv = MAX_COST, cost_fill = MAX_COST;
    uint32_t cost_raw, cost_split = 0, best;
    uint8_t fill_color = 0;
    int x = mbx * 8, y = mby * 8;
    int mvx = 0, mvy = 0, sub, i;

    region_save(x, y, 8, save);

    if (!keyframe) {
        uint32_t e = region_err(x, y, 8);
        if (e == 0) {
            pred->dx = pred->dy = 0;
            return -1;   /* perfectly unchanged: nothing beats a skip run */
        }
        cost_skip = e + lambda / 2;   /* a skip run costs well under a byte */
        cost_mv = motion_search(x, y, 8, save, pred->dx, pred->dy, &mvx, &mvy);
        if (cost_mv != MAX_COST) {
            cost_mv += lambda * 2;
        }
    }

    cost_fill = pick_fill(x, y, 8, &fill_color) + lambda * 2;
    cost_raw = lambda * 65;

    /* The split has to be evaluated by actually building it, because each
     * sub-block's options depend on the ones before it. */
    split.len = 0;
    cost_split = lambda;   /* the MBV_MB_SPLIT opcode byte */
    for (sub = 0; sub < 4; sub++) {
        int bx = x + ((sub & 1) << 2);
        int by = y + ((sub & 2) << 1);
        cost_split += code_block(bx, by, lambda, keyframe, mvx, mvy, &split);
    }

    best = cost_split;
    if (cost_skip < best) best = cost_skip;
    if (cost_mv   < best) best = cost_mv;
    if (cost_fill < best) best = cost_fill;
    if (cost_raw  < best) best = cost_raw;

    /* Ties go to whichever is cheapest for the machine to decode, which is
     * also the order these are tested in. */
    if (best == cost_split && cost_skip > best && cost_mv > best &&
        cost_fill > best && cost_raw > best) {
        put8(o, MBV_MB_SPLIT);
        for (i = 0; i < split.len; i++) {
            put8(o, split.buf[i]);
        }
        pred->dx = mvx;
        pred->dy = mvy;
        return MBV_MB_SPLIT;
    }

    /* Anything else replaces the whole macroblock, so undo the trial split. */
    region_restore(x, y, 8, save);

    if (best == cost_skip) {
        pred->dx = pred->dy = 0;
        return -1;
    }
    if (best == cost_mv) {
        put8(o, MBV_MB_MOTION);
        put8(o, MBV_MV_PACK(mvx, mvy));
        fmt_mbv_motion(g_recon, W, x, y, mvx, mvy, 8);
        pred->dx = mvx;
        pred->dy = mvy;
        return MBV_MB_MOTION;
    }
    if (best == cost_fill) {
        put8(o, MBV_MB_FILL);
        put8(o, fill_color);
        fmt_mbv_fill(g_recon + (size_t)y * W + x, W, 8, fill_color);
        pred->dx = pred->dy = 0;
        return MBV_MB_FILL;
    }

    put8(o, MBV_MB_RAW);
    for (i = 0; i < 8; i++) {
        const uint8_t *t = g_target + (size_t)(y + i) * W + x;
        int j;
        for (j = 0; j < 8; j++) {
            put8(o, t[j]);
        }
        memcpy(g_recon + (size_t)(y + i) * W + x, t, 8);
    }
    pred->dx = pred->dy = 0;
    return MBV_MB_RAW;
}

/* ------------------------------------------------------------------ */
/* Frame assembly                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
} vbuf;

static void vb_put(vbuf *v, const uint8_t *p, size_t n)
{
    if (v->len + n > v->cap) {
        v->cap = (v->len + n) * 2 + 4096;
        v->buf = realloc(v->buf, v->cap);
        if (!v->buf) {
            die("out of memory");
        }
    }
    memcpy(v->buf + v->len, p, n);
    v->len += n;
}

static void vb_put8(vbuf *v, unsigned c)
{
    uint8_t b = (uint8_t)c;
    vb_put(v, &b, 1);
}

/* Encodes the video payload of one frame into `v`. */
static void encode_video(int keyframe, uint32_t lambda, vbuf *v)
{
    mv_t pred = { 0, 0 };
    int mbx, mby, run = 0;

    v->len = 0;
    for (mby = 0; mby < MBH; mby++) {
        for (mbx = 0; mbx < MBW; mbx++) {
            obuf o;
            int op;

            o.len = 0;
            op = code_macroblock(mbx, mby, lambda, keyframe, &pred, &o);
            if (op < 0) {
                if (++run == (int)MBV_MB_SKIP_MAX) {
                    vb_put8(v, (unsigned)(run - 1));
                    run = 0;
                }
                continue;
            }
            if (run) {
                vb_put8(v, (unsigned)(run - 1));
                run = 0;
            }
            vb_put(v, o.buf, (size_t)o.len);
        }
    }
    if (run) {
        vb_put8(v, (unsigned)(run - 1));
    }
}

static void put_le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
"usage: mbvenc [options] video.rgb24 audio.u8 out.mbv\n"
"  -w N     frame width (default 256, multiple of 8)\n"
"  -h N     frame height (default 240, multiple of 8)\n"
"  -f FPS   frame rate (default 12)\n"
"  -r HZ    audio sample rate (default 16000, unsigned 8-bit mono)\n"
"  -g N     keyframe/palette interval in frames (default 24)\n"
"  -b N     byte budget per inter frame (default 5000)\n"
"  -k N     byte budget per keyframe (default 20000)\n"
"  -m N     hard chunk ceiling in bytes (default 30000)\n"
"  -s N     motion search radius in pixels, 0 disables (default 8)\n"
"  -D FILE  dump the encoder's reconstruction (768-byte palette plus one\n"
"           byte per pixel, per frame) for tests/mbv_roundtrip_test.c\n"
"  -q       quiet\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *vpath = NULL, *apath = NULL, *opath = NULL;
    FILE *vf, *af, *of, *df = NULL;
    long vbytes;
    uint32_t nframes, frame, audio_done = 0, max_chunk = 0, total_bytes = 0;
    uint32_t lambda = 3000;
    size_t frame_rgb;
    uint8_t hdr[MBV_HEADER_BYTES];
    uint8_t *audio_buf;
    vbuf video = { NULL, 0, 0 };
    int i, fps_q8;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') {
            if (!vpath)      vpath = a;
            else if (!apath) apath = a;
            else if (!opath) opath = a;
            else usage();
            continue;
        }
        if (!strcmp(a, "-q")) { opt_quiet = 1; continue; }
        if (i + 1 >= argc) usage();
        switch (a[1]) {
        case 'w': opt_w = atoi(argv[++i]); break;
        case 'h': opt_h = atoi(argv[++i]); break;
        case 'f': opt_fps = atof(argv[++i]); break;
        case 'r': opt_audio_rate = atoi(argv[++i]); break;
        case 'g': opt_gop = atoi(argv[++i]); break;
        case 'b': opt_budget = atoi(argv[++i]); break;
        case 'k': opt_key_budget = atoi(argv[++i]); break;
        case 'm': opt_max_chunk = atoi(argv[++i]); break;
        case 's': opt_search = atoi(argv[++i]); break;
        case 'D': opt_dump = argv[++i]; break;
        default: usage();
        }
    }
    if (!opath) {
        usage();
    }
    if ((opt_w & 7) || (opt_h & 7) || opt_w <= 0 || opt_h <= 0) {
        die("width and height must be positive multiples of 8");
    }
    fps_q8 = (int)(opt_fps * 256.0 + 0.5);
    if (fps_q8 <= 0 || fps_q8 > 65535) {
        die("frame rate out of range");
    }
    if (opt_gop < 1) {
        opt_gop = 1;
    }
    if (opt_search > 8) {
        opt_search = 8;   /* the vector nibble tops out at +7/-8 */
    }

    W = opt_w;
    H = opt_h;
    MBW = W / 8;
    MBH = H / 8;
    frame_rgb = (size_t)W * H * 3;

    vf = fopen(vpath, "rb");
    if (!vf) die("cannot open video input");
    af = fopen(apath, "rb");
    if (!af) die("cannot open audio input");
    of = fopen(opath, "wb");
    if (!of) die("cannot open output");
    if (opt_dump) {
        df = fopen(opt_dump, "wb");
        if (!df) die("cannot open reconstruction dump");
    }

    fseek(vf, 0, SEEK_END);
    vbytes = ftell(vf);
    fseek(vf, 0, SEEK_SET);
    nframes = (uint32_t)(vbytes / (long)frame_rgb);
    if (!nframes) {
        die("video input holds no whole frames at this size");
    }

    g_rgb        = xalloc(frame_rgb * (size_t)opt_gop);
    g_target     = xalloc((size_t)W * H);
    g_recon      = xalloc((size_t)W * H);
    g_recon_save = xalloc((size_t)W * H);
    g_dist       = xalloc(sizeof(uint32_t) * PAL_SIZE * PAL_SIZE);
    g_map15      = xalloc(32768);
    audio_buf    = xalloc(65536);

    memset(hdr, 0, sizeof(hdr));
    fwrite(hdr, 1, sizeof(hdr), of);

    for (frame = 0; frame < nframes; frame++) {
        int gop_pos = (int)(frame % (uint32_t)opt_gop);
        int keyframe = (gop_pos == 0);
        uint32_t want_audio, alen, budget, chunk_len;
        size_t got;
        uint8_t chdr[MBV_CHUNK_HEADER_BYTES];
        int tries;

        if (keyframe) {
            /* Read the whole GOP so its palette can be built from all of it,
             * then rewind to encode the frames one at a time. */
            uint32_t n = nframes - frame;
            if (n > (uint32_t)opt_gop) {
                n = (uint32_t)opt_gop;
            }
            got = fread(g_rgb, 1, frame_rgb * n, vf);
            if (got != frame_rgb * n) {
                die("short read on video input");
            }
            fseek(vf, (long)(frame * frame_rgb), SEEK_SET);
            build_palette(g_rgb, (int)n);
            build_tables();
        }
        got = fread(g_rgb, 1, frame_rgb, vf);
        if (got != frame_rgb) {
            die("short read on video input");
        }
        quantise_frame(g_rgb, g_target);

        /* Audio for exactly this frame, so that playing the stream at the
         * DAC's rate reproduces the declared frame rate with no drift: the
         * player takes the audio as its clock. */
        want_audio = (uint32_t)(((uint64_t)(frame + 1) * opt_audio_rate * 256u)
                                / (uint32_t)fps_q8) - audio_done;
        if (want_audio > 65535u) {
            want_audio = 65535u;
        }
        alen = (uint32_t)fread(audio_buf, 1, want_audio, af);
        if (alen < want_audio) {
            memset(audio_buf + alen, 0x80, want_audio - alen);  /* silence */
        }
        alen = want_audio;
        audio_done += alen;

        budget = (uint32_t)(keyframe ? opt_key_budget : opt_budget);
        memcpy(g_recon_save, g_recon, (size_t)W * H);

        for (tries = 0; ; tries++) {
            encode_video(keyframe, lambda, &video);
            chunk_len = (uint32_t)(MBV_CHUNK_HEADER_BYTES + video.len + alen
                                   + (keyframe ? 768u : 0u));
            if (chunk_len <= (uint32_t)opt_max_chunk || tries >= 8) {
                break;
            }
            /* Over the hard ceiling the player's scratch buffer can hold.
             * Throw the frame away and redo it much more cheaply. */
            lambda *= 3;
            memcpy(g_recon, g_recon_save, (size_t)W * H);
        }

        put_le32(chdr, chunk_len - MBV_CHUNK_HEADER_BYTES);
        put_le16(chdr + 4, alen);
        chdr[6] = (uint8_t)(keyframe ? MBV_FTYPE_KEY : MBV_FTYPE_INTER);
        chdr[7] = 0;
        fwrite(chdr, 1, sizeof(chdr), of);
        if (keyframe) {
            fwrite(g_pal, 1, 768, of);
        }
        fwrite(audio_buf, 1, alen, of);
        fwrite(video.buf, 1, video.len, of);
        if (df) {
            /* What the machine will be holding once it has decoded this
             * chunk.  The test re-decodes the stream and compares. */
            fwrite(g_pal, 1, 768, df);
            fwrite(g_recon, 1, (size_t)W * H, df);
        }

        if (chunk_len > max_chunk) {
            max_chunk = chunk_len;
        }
        total_bytes += chunk_len;

        /* Rate control.  A plain proportional step on lambda, clamped, run
         * once per frame - the budget is a bandwidth ceiling, not a target to
         * hit precisely, so there is nothing to gain from converging harder. */
        if (chunk_len > budget) {
            uint32_t over = chunk_len - budget;
            uint32_t up = 100u + (over > budget ? 60u : 60u * over / budget);
            lambda = lambda * up / 100u;
        } else {
            uint32_t under = budget - chunk_len;
            uint32_t down = 100u - (under > budget / 2u ? 25u : 50u * under / budget);
            lambda = lambda * down / 100u;
        }
        if (lambda < 100u)     lambda = 100u;
        if (lambda > 4000000u) lambda = 4000000u;

        if (!opt_quiet && (frame % 25u == 0 || keyframe)) {
            printf("frame %5u/%u  %s %6u bytes  lambda %u\n",
                   frame, nframes, keyframe ? "KEY " : "    ",
                   chunk_len, lambda);
            fflush(stdout);
        }
    }

    memcpy(hdr, MBV_MAGIC, 4);
    put_le16(hdr + 4, (unsigned)W);
    put_le16(hdr + 6, (unsigned)H);
    put_le16(hdr + 8, (unsigned)fps_q8);
    hdr[10] = 8;
    hdr[11] = MBV_FLAG_AUDIO;
    put_le32(hdr + 12, nframes);
    put_le32(hdr + 16, (uint32_t)opt_audio_rate);
    put_le32(hdr + 20, max_chunk);
    put_le32(hdr + 24, audio_done);
    fseek(of, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), of);

    if (df) {
        fclose(df);
    }
    fclose(of);
    fclose(vf);
    fclose(af);

    if (!opt_quiet) {
        double secs = (double)nframes * 256.0 / (double)fps_q8;
        printf("%u frames, %ux%u, %.2f fps, %u bytes total"
               " (%.1f KB/s, largest chunk %u)\n",
               nframes, W, H, (double)fps_q8 / 256.0,
               total_bytes + MBV_HEADER_BYTES,
               (double)total_bytes / secs / 1024.0, max_chunk);
    }
    return 0;
}
