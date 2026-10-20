/*
 * Minimal PNG file writing (see png_writer.h).
 */
/* Standard C functions (fopen, getenv, strtok): no "unsafe" warning from MSVC. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "png_writer.h"

#include "gcrt_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_crc_table[256];
static bool s_crc_ready = false;

static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) {
      c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    s_crc_table[n] = c;
  }
  s_crc_ready = true;
}

static uint32_t crc_update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc = s_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc;
}

static void put_be32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static bool write_chunk(FILE* f, const char type[4], const uint8_t* data, uint32_t len) {
  uint8_t head[8];
  put_be32(head, len);
  memcpy(head + 4, type, 4);
  uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t*)type, 4);
  if (len != 0) {
    crc = crc_update(crc, data, len);
  }
  crc ^= 0xFFFFFFFFu;
  uint8_t tail[4];
  put_be32(tail, crc);
  return fwrite(head, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) && fwrite(tail, 1, 4, f) == 4;
}

bool gcrt_png_write_rgba(const char* path, const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t stride) {
  if (path == NULL || rgba == NULL || width == 0 || height == 0) {
    return false;
  }
  if (!s_crc_ready) {
    crc_init();
  }

  /* Raw data: one filter byte (0 = none) per row, then the RGBA row. */
  const size_t row = (size_t)width * 4u + 1u;
  const size_t raw_len = row * height;
  /* Stored DEFLATE blocks: 5 header bytes per block of at most 65535 bytes. */
  const size_t nblocks = (raw_len + 65534u) / 65535u;
  const size_t z_len = 2u + raw_len + nblocks * 5u + 4u;
  if (z_len > 0x7FFFFFFFu) {
    return false;
  }
  uint8_t* z = (uint8_t*)malloc(z_len);
  uint8_t* raw = (uint8_t*)malloc(raw_len);
  if (z == NULL || raw == NULL) {
    free(z);
    free(raw);
    return false;
  }
  for (uint32_t y = 0; y < height; y++) {
    raw[y * row] = 0;
    memcpy(raw + y * row + 1, rgba + (size_t)y * stride, (size_t)width * 4u);
  }

  size_t o = 0;
  z[o++] = 0x78; /* CMF: deflate, 32 KiB window */
  z[o++] = 0x01; /* FLG: (0x78 << 8 | 0x01) % 31 == 0, no dictionary */
  uint32_t a = 1, b = 0; /* Adler-32 */
  size_t done = 0;
  while (done < raw_len) {
    size_t n = raw_len - done;
    if (n > 65535u) {
      n = 65535u;
    }
    const bool last = done + n == raw_len;
    z[o++] = last ? 1u : 0u;
    z[o++] = (uint8_t)(n & 0xFFu);
    z[o++] = (uint8_t)(n >> 8);
    z[o++] = (uint8_t)(~n & 0xFFu);
    z[o++] = (uint8_t)((~n >> 8) & 0xFFu);
    memcpy(z + o, raw + done, n);
    for (size_t i = 0; i < n; i++) {
      a = (a + raw[done + i]) % 65521u;
      b = (b + a) % 65521u;
    }
    o += n;
    done += n;
  }
  put_be32(z + o, (b << 16) | a);
  o += 4;
  free(raw);

  FILE* f = gcrt_fopen_utf8(path, "wb"); /* UTF-8 path (e.g. GCRT_CAPTURE_DIR with non-ANSI characters) */
  if (f == NULL) {
    free(z);
    return false;
  }
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  uint8_t ihdr[13];
  put_be32(ihdr, width);
  put_be32(ihdr + 4, height);
  ihdr[8] = 8;  /* bit depth */
  ihdr[9] = 6;  /* RGBA */
  ihdr[10] = 0; /* deflate compression */
  ihdr[11] = 0; /* standard filtering */
  ihdr[12] = 0; /* not interlaced */
  bool ok = fwrite(sig, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) &&
            write_chunk(f, "IDAT", z, (uint32_t)o) && write_chunk(f, "IEND", NULL, 0);
  ok = (fclose(f) == 0) && ok;
  free(z);
  return ok;
}
