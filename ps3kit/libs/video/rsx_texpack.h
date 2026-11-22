/*
 * ps3recomp - texture packs (dump + replace), 2026-11-15
 *
 * LD_TEX_DUMP=<dir>: every distinct data texture the game uploads is written
 *   once as <dir>\<hash16hex>.png (RGBA8, top mip only).
 * LD_TEX_PACK=<dir>: a texture whose <hash16hex>.png is in <dir> (indexed once
 *   at start) is drawn from that file instead; any size, mips are generated.
 *
 * The hash is XXH64 of the top mip's source bytes in guest memory, then the
 * base format, width, height (and the remap for formats whose decode bakes it
 * in), so it does not depend on the render scale, the address or the run.
 *
 * What a PNG holds:
 *   - DXT1/3/5: the stored channels (BC decoded); the game's channel remap is
 *     still applied by the sampler view, as for the original.
 *   - the other 8-bit formats: the RGBA the decoder produces (remap applied).
 * Both are uploaded as R8G8B8A8_UNORM, the same view domain as the original,
 * so gamma (applied by the shaders) is unchanged.
 * Not replaced (logged once each): cube maps, volumes, float, depth, X16, and
 * unnormalized-coordinate textures whose replacement is not the same size.
 */
#ifndef RSX_TEXPACK_H
#define RSX_TEXPACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0: neither switch set (every other call is then never made). */
int rsx_texpack_enabled(void);
int rsx_texpack_dump_on(void);
int rsx_texpack_pack_on(void);

/* rows of row_bytes each, stride bytes apart, plus the description. */
uint64_t rsx_texpack_hash(const uint8_t* src, uint32_t row_bytes, uint32_t rows, uint32_t stride,
                          uint32_t base_fmt, uint32_t w, uint32_t h, uint32_t remap);

/* A replacement's RGBA8 mip chain; valid until the next rsx_texpack_find. */
typedef struct {
    uint32_t n;                 /* levels */
    uint32_t w[16], h[16];
    const uint8_t* data[16];    /* tightly packed rows, w*4 bytes each */
} rsx_texpack_image;

/* 1 and *out when the pack has this hash and it can be used. orig_mips: the
 * original's level count (the replacement gets that many plus one per 2x of
 * its scale). same_size_only: unnormalized coordinates (other sizes skipped).
 * max_bytes: the upload limit for the whole chain (halved until it fits). */
/* add 1: AddRef, 0: Release (the GPU texture is opaque here). */
typedef void (*rsx_texpack_ref_fn)(void* gpu, int add);
/* Either *gpu (a texture kept from an earlier upload, already AddRef'd for
 * the caller) or *out (pixels to upload; then call rsx_texpack_keep). */
int rsx_texpack_find(uint64_t hash, uint32_t orig_w, uint32_t orig_h, uint32_t orig_mips,
                     int same_size_only, uint64_t max_bytes, const rsx_texpack_image** out,
                     void** gpu, rsx_texpack_ref_fn ref);
/* The texture made from *out: one reference is kept, the pixels are freed. */
void rsx_texpack_keep(uint64_t hash, uint32_t orig_w, uint32_t orig_h, uint32_t orig_mips, void* gpu);
/* 1: a texture this module holds (its memory is counted by LD_TEX_PACK_MB, not by the
 * texture cache's LD_TEX_CACHE_MB budget). */
int rsx_texpack_owns(const void* gpu);

/* Dump (copies the pixels; written by a background thread). bc: 1/2/3 for
 * DXT1/DXT23/DXT45 blocks (bw*bh blocks, row by row), 0 for RGBA8 rows.
 * loc/off/frame: to stop dumping a texture rewritten every few frames. */
void rsx_texpack_dump(uint64_t hash, int bc, const uint8_t* data, uint32_t w, uint32_t h,
                      uint32_t loc, uint32_t off, uint32_t frame);

/* Log once: a texture kind that is never replaced. */
void rsx_texpack_skip_note(const char* why, uint32_t base_fmt);

#ifdef __cplusplus
}
#endif

#endif
