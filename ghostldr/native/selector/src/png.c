/*
 * PNG decoding with the console's own libScePngDec (a PS4 system module in
 * /system/common/lib), so the module carries no decoder.  The module loads it
 * when SWRR has not, and unloads it before returning.  Each image gets its own
 * decoder in temporary heap memory (QueryMemorySize / Create / Decode /
 * Delete), freed with the image.
 */
#include "plat.h"

void *memset(void *dst, int c, size_t n);

struct png_create {
    u32 this_size, attribute, max_width;
};
struct png_parse {
    const void *data;
    u32 size, reserved;
};
struct png_info {
    u32 width, height;
    u16 color_space, bit_depth;
    u32 flags;
};
struct png_decode {
    const void *data;
    void *image;
    u32 data_size, image_size;
    u16 pixel_format, alpha;
    u32 pitch;
};

#define PNG_PIXELS_MAX (2560u * 1440u)

int png_init(struct plat *P)
{
    u32 h = sys_module(P, "libScePngDec.sprx", 1, &P->mod_png);
    P->png_parse = (s32(*)(const void *, void *))sys_sym(P, h, "scePngDecParseHeader");
    P->png_query = (s32(*)(const void *))sys_sym(P, h, "scePngDecQueryMemorySize");
    P->png_create = (s32(*)(const void *, void *, u32, void **))sys_sym(P, h, "scePngDecCreate");
    P->png_decode = (s32(*)(void *, const void *, void *))sys_sym(P, h, "scePngDecDecode");
    P->png_delete = (s32(*)(void *))sys_sym(P, h, "scePngDecDelete");
    if (P->png_parse && P->png_query && P->png_create && P->png_decode && P->png_delete)
        return 0;
    P->png_decode = 0;
    return -1;
}

/* Decode into a temporary BGRA surface (B8G8R8A8: VideoOut's byte order). */
int png_decode_to(struct sel *S, struct plat *P, const u8 *data, u64 n, surface *out)
{
    struct png_parse pp = {data, (u32)n, 0};
    struct png_info info;
    memset(&info, 0, sizeof(info));
    s32 rc = P->png_parse(&pp, &info);
    if (rc < 0 || !info.width || !info.height || (u64)info.width * info.height > PNG_PIXELS_MAX) {
        sel_log(S, "PNG header %x: %ux%u", (u32)rc, info.width, info.height);
        return -1;
    }
    struct png_create cp = {sizeof(cp), 0, info.width};
    s32 mem = P->png_query(&cp);
    void *work = mem > 0 ? sel_alloc(S, (u64)mem) : NULL;
    void *handle = NULL;
    if (!work || (rc = P->png_create(&cp, work, (u32)mem, &handle)) < 0) {
        sel_log(S, "scePngDecCreate (%d bytes): %x", mem, (u32)rc);
        return -1;
    }
    u64 bytes = (u64)info.width * info.height * 4;
    u32 *px = sel_alloc(S, bytes);
    struct png_decode dp = {data, px, (u32)n, (u32)bytes, 1 /* B8G8R8A8 */, 255, info.width * 4};
    struct png_info got;
    memset(&got, 0, sizeof(got));
    rc = px ? P->png_decode(handle, &dp, &got) : -1;
    P->png_delete(handle);
    if (rc < 0) {
        sel_log(S, "scePngDecDecode %ux%u: %x", info.width, info.height, (u32)rc);
        return -1;
    }
    out->px = px, out->w = (int)info.width, out->h = (int)info.height, out->stride = (int)info.width;
    return 0;
}
