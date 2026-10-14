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

bool gcrt_disc_image_path(const char* disc_root, char* out, size_t size) {
    const char* env = getenv("GCRT_DISC_IMAGE");
    if (env && env[0]) {
        snprintf(out, size, "%s", env);
        return true;
    }
    if (!disc_root)
        return false;
    /* <disc_root>/../manifest.json, "image" field (written by tools/extract.py; parsed without a library). */
    char path[1024];
    snprintf(path, sizeof(path), "%s/../manifest.json", disc_root);
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    const char* key = strstr(text, "\"image\"");
    if (!key)
        return false;
    const char* colon = strchr(key + 7, ':');
    const char* quote = colon ? strchr(colon, '"') : NULL;
    if (!quote)
        return false;
    size_t used = 0;
    for (const char* p = quote + 1; *p && *p != '"' && used + 1 < size; p++) {
        if (*p == '\\' && p[1])
            p++;
        out[used++] = *p;
    }
    out[used] = '\0';
    return used > 0;
}
