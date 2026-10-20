/* Installation of the native replacements (milestone 10). See gcrt_native.h.
 *
 * Everything is native since step 10d (no emulated hardware any more; reference: the last build that still emulated
 * the hardware, kept outside this repository's public history): disc (dvd), controllers (pad), display (vi), caches
 * (cache), audio memory (ar) since step 10a, memory card (card) since milestone 7, drawing (gx) since 10c, DSP and
 * AI (dsp, ai) since 10d; the OS since 10b. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/gcrt_native.h"
#include "hle/gcrt_native_modules.h"
#include "platform/gcrt_path.h"
#include "platform/gcrt_save_file.h"
#include "platform/trace.h"

#include <stdlib.h>
#include <string.h>

/* Subsystems, all native: their emulated version has been removed. */
static const char* const k_native[] = {"dvd", "pad", "vi", "cache", "card", "gx", "ar", "dsp", "ai"};
#define NATIVE_COUNT (sizeof(k_native) / sizeof(k_native[0]))

const char* gcrt_native_describe(char* buf, size_t size) {
    size_t used = 0;
    int n = snprintf(buf, size, "native:");
    if (n > 0 && (size_t)n < size)
        used = (size_t)n;
    for (size_t i = 0; i < NATIVE_COUNT; i++) {
        n = snprintf(buf + used, size - used, " %s", k_native[i]);
        if (n > 0 && (size_t)n < size - used)
            used += (size_t)n;
    }
    n = snprintf(buf + used, size - used, "; emulated: none");
    (void)n;
    return buf;
}

void gcrt_native_install(GcrtCpu* g, const char* disc_root) {
    gcrt_trace_init();
    char text[256];
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "subsystems: %s", gcrt_native_describe(text, sizeof(text)));
    gcrt_nirq_init(g);
    /* Milestone 10b: native OS (threads, contexts, FPU, idle) then alarms; the other replacements rely on them. */
    gcrt_native_os_install(g);
    gcrt_native_os_alarm_install(g);
    gcrt_native_os_sys_install(g);
    gcrt_native_dvd_install(g, disc_root);
    gcrt_native_pad_install(g);
    gcrt_native_ar_install(g);
    gcrt_native_audio_install(g);
    gcrt_native_vi_install(g);
    gcrt_native_cache_install(g);
    gcrt_native_card_install(g);
    gcrt_native_gx_install(g);
}

void gcrt_native_report(FILE* out) {
    char text[256];
    fprintf(out, "subsystems: %s\n", gcrt_native_describe(text, sizeof(text)));
    gcrt_nirq_report(out);
    gcrt_native_os_report(out);
    gcrt_native_os_alarm_report(out);
    gcrt_native_os_sys_report(out);
    gcrt_native_dvd_report(out);
    gcrt_native_pad_report(out);
    gcrt_native_ar_report(out);
    gcrt_native_audio_report(out);
    gcrt_native_vi_report(out);
    gcrt_native_cache_report(out);
    gcrt_native_card_report(out);
    gcrt_native_gx_report(out);
}

/* ---------------------------------------------------------------------------------------------
 * Disc image
 * ------------------------------------------------------------------------------------------- */

/* Appends the UTF-8 encoding of code point `c` to out[*used]; false if it does not fit. */
static bool put_utf8(char* out, size_t size, size_t* used, unsigned long c) {
    char b[4];
    size_t n;
    if (c < 0x80) {
        b[0] = (char)c, n = 1;
    } else if (c < 0x800) {
        b[0] = (char)(0xC0 | (c >> 6)), b[1] = (char)(0x80 | (c & 0x3F)), n = 2;
    } else if (c < 0x10000) {
        b[0] = (char)(0xE0 | (c >> 12)), b[1] = (char)(0x80 | ((c >> 6) & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (c >> 18)), b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    if (*used + n + 1 > size)
        return false;
    memcpy(out + *used, b, n);
    *used += n;
    return true;
}

static int hex4(const char* p, unsigned long* v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return 0;
        *v = (*v << 4) | (unsigned long)d;
    }
    return 1;
}

/* Value of the JSON string field `key` of `text` (UTF-8, escapes \" \\ \/ \b \f \n \r \t \uXXXX decoded); parsed
 * without a library: the manifest is small and written by our tools (tools/extract.py, tools/gcextract). */
static bool json_string_field(const char* text, const char* key, char* out, size_t size) {
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char* k = strstr(text, quoted);
    if (!k)
        return false;
    const char* colon = strchr(k + strlen(quoted), ':');
    const char* quote = colon ? strchr(colon, '"') : NULL;
    if (!quote)
        return false;
    size_t used = 0;
    for (const char* p = quote + 1; *p && *p != '"'; p++) {
        unsigned long c = (unsigned char)*p;
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned long lo;
                if (!hex4(p + 1, &c))
                    return false;
                p += 4;
                if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u' && hex4(p + 3, &lo) && lo >= 0xDC00 &&
                    lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                if (!put_utf8(out, size, &used, c))
                    return false;
                continue;
            }
            default: c = (unsigned char)*p; break; /* \" \ \/ */
            }
        }
        if (used + 2 > size)
            return false;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return used > 0;
}

/* Last component of a path ("a/b\\c.iso" -> "c.iso"). */
static const char* file_name_of(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            name = p + 1;
    return name;
}

bool gcrt_disc_image_locate(const char* disc_root, char* out, size_t size, GcrtDiscImageSource* source) {
    GcrtDiscImageSource dummy;
    if (!source)
        source = &dummy;
    if (size)
        out[0] = '\0';
    if (gcrt_getenv_utf8("GCRT_DISC_IMAGE", out, size)) {
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "disc image: %s (GCRT_DISC_IMAGE)", out);
        *source = GCRT_DISC_IMAGE_FROM_ENV;
        return true;
    }
    *source = GCRT_DISC_IMAGE_NO_MANIFEST;
    if (!disc_root)
        return false;
    /* <disc_root>/../manifest.json, "image" field (written by tools/extract.py or tools/gcextract --manifest). */
    char folder[1100], path[1200];
    snprintf(folder, sizeof(folder), "%s/..", disc_root);
    snprintf(path, sizeof(path), "%s/manifest.json", folder);
    snprintf(out, size, "%s", path); /* on failure, `out` names the manifest */
    FILE* f = gcrt_fopen_utf8(path, "rb");
    if (!f) {
        const bool exists = gcrt_sf_is_file(path);
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "disc image: %s %s", exists ? "unreadable manifest" : "no manifest", path);
        *source = exists ? GCRT_DISC_IMAGE_BAD_MANIFEST : GCRT_DISC_IMAGE_NO_MANIFEST;
        return false;
    }
    char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    char image[1024];
    if (!json_string_field(text, "image", image, sizeof(image))) {
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "disc image: no \"image\" field in %s", path);
        *source = GCRT_DISC_IMAGE_BAD_MANIFEST;
        return false;
    }
    *source = GCRT_DISC_IMAGE_FROM_MANIFEST;
    /* A relative "image" is relative to the manifest's folder: an installed copy can be moved or renamed. */
    if (!gcrt_path_is_absolute(image)) {
        snprintf(out, size, "%s/%s", folder, image);
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "disc image: %s (manifest %s, relative)", out, path);
        return true;
    }
    snprintf(out, size, "%s", image);
    if (!gcrt_sf_is_file(image)) {
        /* Absolute path that no longer exists (folder moved with its image): the same file name next to the
         * manifest, if it is there. */
        char beside[1200];
        snprintf(beside, sizeof(beside), "%s/%s", folder, file_name_of(image));
        if (gcrt_sf_is_file(beside)) {
            gcrt_trace_printf(GCRT_TRACE_NATIVE, "disc image: %s missing, %s used (next to the manifest)", image,
                              beside);
            snprintf(out, size, "%s", beside);
            return true;
        }
    }
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "disc image: %s (manifest %s)", out, path);
    return true;
}

bool gcrt_disc_image_path(const char* disc_root, char* out, size_t size) {
    if (gcrt_disc_image_locate(disc_root, out, size, NULL))
        return true;
    if (size)
        out[0] = '\0';
    return false;
}
