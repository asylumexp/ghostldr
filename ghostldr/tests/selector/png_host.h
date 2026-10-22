/* PNG reading and writing for the selector's offline harnesses (zlib). */
#pragma once
/* Decode a PNG (8/16-bit gray, RGB, palette, with alpha; not interlaced).
 * out NULL: only the size.  Pixels are 0xAARRGGBB.  0 on success. */
int png_host_decode(const unsigned char *data, unsigned long n, unsigned *w, unsigned *h, unsigned *out);
/* Write 0xAARRGGBB pixels (stride in pixels) as an RGB PNG.  0 on success. */
int png_host_write(const char *path, const unsigned *px, int w, int h, int stride);
