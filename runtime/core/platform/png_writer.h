/*
 * Minimal PNG file writing (8-bit RGBA), with no dependency.
 *
 * Generic: used for the runtime's automatic screenshots (comparison
 * with Dolphin) and for prototypes. The data is written in "stored"
 * (uncompressed) DEFLATE blocks: bigger files, but short and safe code.
 * Format: PNG specification (W3C, 2nd edition), zlib (RFC 1950), DEFLATE (RFC 1951).
 */
#ifndef GCRT_PLATFORM_PNG_WRITER_H
#define GCRT_PLATFORM_PNG_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Writes `rgba` (width * height pixels, 4 bytes R,G,B,A per pixel, rows of
 * `stride` bytes) to `path`. Returns true if the file was written.
 */
bool gcrt_png_write_rgba(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height,
                         uint32_t stride);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_PLATFORM_PNG_WRITER_H */
