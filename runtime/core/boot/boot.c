/* HLE boot and entry point of the generic GameCube runtime.
 *
 * On the console, the IPL (BS2) prepares the processor and low memory, loads the apploader from the disc and
 * runs it: the apploader is what loads the DOL and the FST and fills in the low globals that depend on them.
 * We prepare the processor and low memory like Dolphin's HLE boot (Core/Boot/Boot_BS2Emu.cpp:
 * SetupMSR, SetupHID, SetupBAT, SetupGCMemory, DVDReadDiscID; repository dolphin-emu/dolphin, commit
 * e6f3ae17627e4344da95b13424af5baf4c892b08, GPL-2.0-or-later; values reused, no code copied), then we directly
 * set what the apploader would have written (static boot). Milestone 10b: the disc's apploader is no longer
 * run (no more mini-interpreter); the values set were identical to those of the apploader of the first game
 * ported (verified with --dump before milestone 10b), except 0x800030D4, which that game does not read. General rule
 * [hypothesis] for another game. See notes/hle.md (section 5, boot and low globals) and the internal boot notes. */
#include "boot/gcrt_boot.h"
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native.h"
#include "platform/gcrt_console.h"
#include "platform/gcrt_crash.h"
#include "platform/gcrt_fatal.h"
#include "platform/gcrt_game_config.h"
#include "platform/gcrt_path.h"
#include "platform/gcrt_platform.h"
#include "platform/gcrt_save_file.h"
#include "platform/trace.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    LOW_DISC_HEADER = 0x80000000u, /* first 0x20 bytes of the disc header */
    LOW_BOOT_MAGIC = 0x80000020u,
    LOW_APPLOADER_VERSION = 0x80000024u, /* 1, written by the apploader */
    LOW_MEM_SIZE = 0x80000028u,
    LOW_CONSOLE_TYPE = 0x8000002Cu,
    LOW_ARENA_LO = 0x80000030u,
    LOW_ARENA_HI = 0x80000034u,
    LOW_FST_ADDRESS = 0x80000038u,
    LOW_FST_MAX_SIZE = 0x8000003Cu,
    LOW_VIDEO_MODE = 0x800000CCu,
    LOW_ARAM_SIZE = 0x800000D0u,
    LOW_MEM_TOP = 0x800000ECu,     /* 0x81800000, written by the apploader */
    LOW_SIM_MEM_SIZE = 0x800000F0u,
    LOW_BI2_POINTER = 0x800000F4u, /* copy of bi2.bin, written by the apploader */
    LOW_BUS_CLOCK = 0x800000F8u,
    LOW_CPU_CLOCK = 0x800000FCu,
    LOW_SYSTEM_TIME = 0x800030D8u, /* u64: system time (time base) */

    BOOT_BIN_FST_OFFSET = 0x424,
    BOOT_BIN_FST_SIZE = 0x428,
    BOOT_BIN_FST_MAX_SIZE = 0x42C,
    BI2_REGION = 0x18,

    SPR_IBAT0U = 528, SPR_IBAT0L = 529, SPR_DBAT0U = 536, SPR_DBAT0L = 537, SPR_DBAT1U = 538, SPR_DBAT1L = 539,
    SPR_HID0 = 1008, SPR_HID1 = 1009,
};

#define GC_EPOCH 0x386D4380u /* 2000-01-01 00:00:00 (Dolphin, HW/EXI/EXI_DeviceIPL.h) */

/* `path` in UTF-8 (platform/gcrt_path.h: wide-character open on Windows). */
static u8* read_file(const char* path, size_t* size_out) {
    FILE* f = gcrt_fopen_utf8(path, "rb");
    if (!f)
        return NULL;
    u8* data = NULL;
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = (u8*)malloc(size ? (size_t)size : 1u);
        if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    if (data)
        *size_out = (size_t)size;
    return data;
}

static void join(char* out, size_t out_size, const char* dir, const char* name) {
    snprintf(out, out_size, "%s/%s", dir, name);
}

/* Console time in seconds since 1 January 2000: GCRT_RTC (fixed clock for reproducible runs, same units
 * as the console clock), otherwise the host's local time (Dolphin,
 * CEXIIPL::GetEmulatedTime(GC_EPOCH)). */
static u64 console_seconds_since_2000(void);

/* Local time in seconds since 1970 (like Dolphin's Common::Timer::GetLocalTimeSinceJan1970). */
static u64 local_seconds_since_1970(void) {
    time_t now = time(NULL);
    struct tm lt = *localtime(&now);
    /* days since 1970-01-01 for (year, month, day): "days from civil" algorithm (H. Hinnant) */
    int y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (u64)(days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
}

static u64 console_seconds_since_2000(void) {
    const char* fixed = getenv("GCRT_RTC");
    if (fixed && fixed[0])
        return strtoull(fixed, NULL, 0);
    return local_seconds_since_1970() - GC_EPOCH;
}

bool gcrt_boot_hle(GcrtCpu* g, const char* disc_root, const GcrtDolInfo* dol) {
    CPUState* cpu = &g->state;
    char path[1024];
    size_t boot_size = 0, bi2_size = 0, fst_size = 0;

    join(path, sizeof(path), disc_root, "sys/boot.bin");
    u8* boot = read_file(path, &boot_size);
    if (!boot || boot_size < 0x440) {
        fprintf(stderr, "gcrt: unreadable disc header: %s\n", path);
        free(boot);
        return false;
    }
    join(path, sizeof(path), disc_root, "sys/bi2.bin");
    u8* bi2 = read_file(path, &bi2_size);
    u32 region = (bi2 && bi2_size >= BI2_REGION + 4) ? read_be32(bi2 + BI2_REGION) : 1u;
    bool pal = region == 2u; /* region code from bi2.bin: 0 Japan, 1 America, 2 Europe (PAL) */

    /* --- Processor (SetupMSR, SetupHID, SetupBAT) [verified: Dolphin] --- */
    cpu->msr = 0x00002000u | 0x00000020u | 0x00000010u | GCRT_MSR_RI; /* FP, IR, DR, RI = 0x2032 */
    g->spr[SPR_HID0] = 0x0011C464u;
    g->spr[SPR_HID1] = 0x80000000u; /* PowerPC.cpp: "running at 3x the bus clock" */
    cpu->hid2 = 0xE0000000u;        /* PSE, WPE, LSQE */
    g->spr[SPR_IBAT0U] = 0x80001FFFu;
    g->spr[SPR_IBAT0L] = 0x00000002u;
    g->spr[SPR_DBAT0U] = 0x80001FFFu;
    g->spr[SPR_DBAT0L] = 0x00000002u;
    g->spr[SPR_DBAT1U] = 0xC0001FFFu;
    g->spr[SPR_DBAT1L] = 0x0000002Au;

    /* --- Low memory (SetupGCMemory, DVDReadDiscID) [verified: Dolphin] --- */
    memcpy(cpu->ram, boot, 0x20);
    mem_write32(cpu, LOW_BOOT_MAGIC, 0x0D15EA5Eu); /* boot from the disc (0xE5207C22: JTAG) */
    mem_write32(cpu, LOW_MEM_SIZE, GCRT_MEM1_SIZE);
    /* Console type: Dolphin writes 0x10000006 (latest development kit) for lack of a better value for some
     * games; we write 0x00000003, the value of retail consoles (Dolphin comment, YAGCD 4.2.1.1.2):
     * deliberate difference, the port reproduces a retail console. */
    mem_write32(cpu, LOW_CONSOLE_TYPE, 0x00000003u);
    mem_write32(cpu, LOW_VIDEO_MODE, pal ? 1u : 0u); /* "fake the VI init of the IPL": NTSC 0, PAL 1 */
    mem_write32(cpu, LOW_ARAM_SIZE, 0x01000000u);   /* 16 MiB of ARAM, no expansion */
    mem_write32(cpu, LOW_BUS_CLOCK, 162000000u);    /* 0x09A7EC80 */
    mem_write32(cpu, LOW_CPU_CLOCK, 486000000u);    /* 0x1CF7C580 */
    mem_write32(cpu, 0x80000300u, 0x4C000064u);     /* default DSI, FPU and system call vectors: rfi */
    mem_write32(cpu, 0x80000800u, 0x4C000064u);
    mem_write32(cpu, 0x80000C00u, 0x4C000064u);
    /* Time base and system time = (console time since 2000-01-01) x 40.5 MHz, like Dolphin: the time base
     * starts from this value (SystemTimers::Init, SetFakeTBStartValue) and 0x800030D8 receives it too
     * (Boot_BS2Emu.cpp, PresetTimeBaseTicks) [verified: Dolphin sources e6f3ae17]. Milestone 10b: before, the time
     * base started from 0 and the game's dates (saves) fell in 2000. */
    const u64 tb_start = console_seconds_since_2000() * (u64)GCRT_TB_HZ;
    mem_write64(cpu, LOW_SYSTEM_TIME, tb_start);
    cpu->timebase = tb_start; /* first synchronization: tb_offset = tb_start (cpu/cpu_core.c) */

    {
        /* Effects of the apploader reproduced statically. Values identical to those written by the apploader of the
         * first game ported (--dump comparison after the apploader, see notes/hle.md); general rule [hypothesis] for
         * another game. Not reproduced: 0x800030D4 (value close to the DOL size, unknown role, not read by the
         * first game ported according to the scan of its DOL). */
        join(path, sizeof(path), disc_root, "sys/fst.bin");
        u8* fst = read_file(path, &fst_size);
        u32 fst_declared = read_be32(boot + BOOT_BIN_FST_SIZE);
        u32 fst_max = read_be32(boot + BOOT_BIN_FST_MAX_SIZE);
        if (fst_max < fst_declared)
            fst_max = fst_declared;
        if (fst && fst_size && fst_max >= fst_size && fst_max < GCRT_MEM1_SIZE / 2) {
            u32 fst_address = (GC_RAM_BASE + GCRT_MEM1_SIZE - fst_max) & ~0x1Fu;
            if (fst_address < dol->highest) {
                fprintf(stderr, "gcrt: the FST (%08X) would overlap the DOL (end %08X)\n", fst_address, dol->highest);
            } else {
                memcpy(cpu->ram + (fst_address - GC_RAM_BASE), fst, fst_size);
                mem_write32(cpu, LOW_FST_ADDRESS, fst_address);
                mem_write32(cpu, LOW_FST_MAX_SIZE, fst_max);
                mem_write32(cpu, LOW_ARENA_HI, fst_address);
                /* Copy of bi2.bin, 0x2000 bytes below the FST, pointer at 0x800000F4 (like the apploader). */
                if (bi2 && bi2_size >= 0x2000u && fst_address - 0x2000u > dol->highest) {
                    memcpy(cpu->ram + (fst_address - 0x2000u - GC_RAM_BASE), bi2, 0x2000u);
                    mem_write32(cpu, LOW_BI2_POINTER, fst_address - 0x2000u);
                }
            }
        } else {
            fprintf(stderr, "gcrt: FST missing or inconsistent (%s): the disc reader will not find any file\n",
                    path);
        }
        free(fst);
        mem_write32(cpu, LOW_ARENA_LO, 0u); /* 0: the OS takes the end of its BSS (__ArenaLo) */
        mem_write32(cpu, LOW_SIM_MEM_SIZE, GCRT_MEM1_SIZE);
        mem_write32(cpu, LOW_APPLOADER_VERSION, 1u);
        mem_write32(cpu, LOW_MEM_TOP, GC_RAM_BASE + GCRT_MEM1_SIZE);
        cpu->gpr[1] = 0x816FFFF0u; /* no effect: __init_registers sets r1, r2 and r13 */
        cpu->pc = dol->entry_point;
    }

    free(boot);
    free(bi2);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------------------------- */

static void usage(const char* exe) {
    printf("Usage: %s [options]\n"
           "  --disc DIR          root of the extracted disc (default: the folder disc next to the executable if it\n"
           "                      holds sys/main.dol, otherwise the one used for the build)\n"
           "  --dol FILE          DOL to load (default: DIR/sys/main.dol)\n"
           "  --max-blocks N      stop after N executed blocks\n"
           "  --max-seconds S     stop after S seconds\n"
           "  --trace FILE        write 'pc lr r1' at each block\n"
           "  --trace-limit N     maximum number of trace lines (default 1000000)\n"
           "  --stop-on-io        stop at the first unhandled hardware access\n"
           "  --io-log N          number of unhandled accesses displayed (default 32)\n"
           "  --stop-on-exception stop at the first synchronous exception (sc included)\n"
           "  --stop-on-fault     stop at the first program, DSI or alignment exception\n"
           "  --clock real|cycles time base: real time (default) or guest cycles (deterministic)\n"
           "  --dump ADDR:LEN     print guest memory when stopping (repeatable, at most 8)\n"
           "  --hle-trace         HLE log (recognized functions, replacements, linked RELs)\n"
           "  --profile           measure host time per category (code, MMIO, clock, host_call...)\n"
           "  --profile-period S  with --profile: one profile line every S seconds (stderr)\n"
           "  --profile-from S    with --profile: sample the guest pc only after S guest seconds\n"
           "  --host-profile      sample the game thread on the host side (Windows, build in RelWithDebInfo)\n"
           "  --guest-log FILE    copy the game's output (OSReport, printf) to a file\n"
           "  --rel-test FILE     diagnostic: link then unlink this .rel with the game's OSLink/OSUnlink\n"
           "  --rel-test-warmup N blocks executed before the REL test (default 30000)\n"
           "  --hw-count FILE     write the hardware access counter as JSON (also GCRT_HW_COUNT_FILE)\n"
           "  --console           open a console window (game output) when the game starts, for this launch;\n"
           "                      the setup menu's \"Show console\" box starts checked (Windows)\n"
           "  --no-console        no console window for this launch; the box starts unchecked (Windows)\n"
           "  --backend NAME      graphics backend for this launch: auto, d3d12 or vulkan (also GCRT_BACKEND;\n"
           "                      default: the setup menu's choice, saved in game_config.ini, else auto). An unknown or\n"
           "                      unavailable name gives auto; a backend that fails to start falls back to auto\n",
           exe);
}

static GcrtCpu g_cpu;

/* ---------------------------------------------------------------------------------------------
 * Start-up checks: errors a player can fix (bad installation, wrong disc image) get a clear message
 * (platform/gcrt_fatal.h: dialog on a player's launch, stderr and trace always, exit code 1) instead of a black window.
 * ------------------------------------------------------------------------------------------- */

enum {
    DISC_HEADER_SIZE = 0x440,       /* boot.bin: the disc header at the start of a GameCube disc image */
    DISC_MAGIC_OFFSET = 0x1C,       /* GameCube disc magic word */
    DISC_MAGIC = 0xC2339F3Du,       /* [verified: bytes 0x1C-0x1F of sys/boot.bin of both supported discs] */
};

static const char* s_title_name = "The game";

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
static void start_error(const char* fmt, ...) {
    char text[6144];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    char title[256];
    snprintf(title, sizeof(title), "%s cannot start", s_title_name);
    gcrt_fatal_error(title, text);
}

/* A disc ID as written in the header: 6 characters, upper-case letters or digits (game code, region, maker). */
static bool disc_id_valid(const char* id) {
    if (!id || strlen(id) != 6)
        return false;
    for (int i = 0; i < 6; i++)
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
            return false;
    return true;
}

/* Disc ID (6 characters + NUL, "" if not valid) and magic test of a disc header (at least 0x20 bytes). */
static void header_id(const u8* header, char id[7], bool* magic) {
    memcpy(id, header, 6);
    id[6] = '\0';
    if (!disc_id_valid(id))
        id[0] = '\0';
    *magic = read_be32(header + DISC_MAGIC_OFFSET) == DISC_MAGIC;
}

static const char k_reinstall[] = "Reinstall the game, or run the installer again and choose your disc image.";

/* The extracted disc header (sys/boot.bin) must exist; its ID must be the executable's when both are known. */
static void check_disc_header(const char* disc, const char* exe_id) {
    char path[1100], shown[2048];
    join(path, sizeof(path), disc, "sys/boot.bin");
    size_t size = 0;
    u8* boot = read_file(path, &size);
    if (!boot || size < DISC_HEADER_SIZE) {
        free(boot);
        fprintf(stderr, "gcrt: unreadable disc header: %s\n", path);
        gcrt_path_display(path, shown, sizeof(shown));
        start_error("The game files are incomplete: the disc header was not found.\n\nThe game looked for:\n%s\n\n%s",
                    shown, k_reinstall);
    }
    char id[7];
    bool magic;
    header_id(boot, id, &magic);
    free(boot);
    if (id[0] && disc_id_valid(exe_id) && strcmp(id, exe_id)) {
        gcrt_path_display(disc, shown, sizeof(shown));
        start_error("This executable is for %s but the game files are %s.\n\nGame files:\n%s\n\n%s", exe_id, id, shown,
                    k_reinstall);
    }
}

/* The disc image (read by the native disc drive, hle/native_dvd.c) must exist and be readable; its ID must be the
 * executable's. Blocks only on a definite problem: an image without the GameCube magic word, or without a valid ID,
 * only gets a warning (development setups). */
static void check_disc_image(const char* disc, const char* exe_id) {
    char path[1024], shown[2048];
    GcrtDiscImageSource source;
    if (!gcrt_disc_image_locate(disc, path, sizeof(path), &source)) {
        gcrt_path_display(path, shown, sizeof(shown));
        fprintf(stderr, "gcrt: disc image not found (%s %s): set GCRT_DISC_IMAGE, or the \"image\" field of "
                        "manifest.json next to the disc folder\n",
                source == GCRT_DISC_IMAGE_BAD_MANIFEST ? "unreadable manifest" : "no manifest", shown);
        if (source == GCRT_DISC_IMAGE_BAD_MANIFEST)
            start_error("The file that names the disc image (manifest.json) cannot be read, or names no image:\n%s\n\n%s",
                        shown, k_reinstall);
        start_error("The disc image was not found: there is no manifest.json next to the game files.\n\nThe game "
                    "looked for:\n%s\n\n%s", shown, k_reinstall);
    }
    gcrt_path_display(path, shown, sizeof(shown));
    const char* via = source == GCRT_DISC_IMAGE_FROM_ENV ? "\n(set by the GCRT_DISC_IMAGE variable)" : "";
    FILE* f = gcrt_fopen_utf8(path, "rb");
    if (!f) {
        if (!gcrt_sf_is_file(path)) {
            fprintf(stderr, "gcrt: disc image not found (\"%s\"): set GCRT_DISC_IMAGE, or the \"image\" field of "
                            "manifest.json next to the disc folder\n", path);
            start_error("The disc image was not found.\n\nThe game looked for:\n%s%s\n\n%s", shown, via, k_reinstall);
        }
        fprintf(stderr, "gcrt: disc image unreadable (\"%s\")\n", path);
        start_error("The disc image cannot be opened:\n%s%s\n\nCheck that no other program is using it and that you "
                    "are allowed to read it, or reinstall the game.", shown, via);
    }
    u8 header[DISC_HEADER_SIZE];
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (n < sizeof(header)) {
        fprintf(stderr, "gcrt: disc image too small (\"%s\", %zu bytes)\n", path, n);
        start_error("The disc image is damaged or incomplete:\n%s%s\n\n%s", shown, via, k_reinstall);
    }
    char id[7];
    bool magic;
    header_id(header, id, &magic);
    if (!magic || !id[0]) {
        fprintf(stderr, "gcrt: warning: %s does not look like a GameCube disc image (%s): the game may not run\n",
                path, magic ? "no valid disc ID" : "no GameCube disc magic");
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "WARNING: disc image %s: %s, disc ID not checked", path,
                          magic ? "no valid disc ID" : "no GameCube disc magic");
        return;
    }
    if (!disc_id_valid(exe_id)) {
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "disc image %s: %s (the executable's ID is unknown: not checked)", path, id);
        return;
    }
    if (strcmp(id, exe_id)) {
        fprintf(stderr, "gcrt: disc image %s is %s, this executable is for %s\n", path, id, exe_id);
        start_error("This executable is for %s but the disc image is %s.\n\nDisc image:\n%s%s\n\nInstall the version of "
                    "the game that matches this disc image, or reinstall with the disc image of %s.", exe_id, id, shown,
                    via, exe_id);
    }
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "disc image %s: %s, matches the executable", path, id);
}

int gcrt_main(int argc, char** argv, const GcrtGameDesc* game) {
    /* Before anything is printed: the console window opens (or not) only when the game starts, and the output written
     * until then is kept for it (platform/gcrt_console.h). */
    gcrt_console_early_init();
    /* Every path of the command line in UTF-8 (on Windows, main's argv is in the ANSI code page): a folder or an
     * image with characters outside that code page can then be opened (platform/gcrt_path.h). */
    gcrt_utf8_args(&argc, &argv);
    gcrt_trace_init();
    const GcrtBuildInfo* info = &gcrt_build_info;
    const char* disc = NULL;
    const char* dol_path = NULL;
    const char* trace_path = NULL;
    GcrtRunLimits limits = {0, 0.0};
    u64 trace_limit = 1000000u;
    bool stop_on_io = false, stop_on_exception = false, stop_on_fault = false;
    u32 io_log = 32;
    GcrtClock clock = GCRT_CLOCK_REALTIME;
    bool hle_trace = false, profile = false;
    double profile_period = 0.0, profile_from = 0.0;
    bool host_profile = false;
    const char* guest_log_path = NULL;
    char hw_count_env[1024];
    const char* hw_count_path =
        gcrt_getenv_utf8("GCRT_HW_COUNT_FILE", hw_count_env, sizeof(hw_count_env)) ? hw_count_env : NULL;
    const char* rel_test_path = NULL;
    u64 rel_test_warmup = 30000u;
    u32 dump_address[8], dump_length[8];
    unsigned dump_count = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(a, "--stop-on-io")) {
            stop_on_io = true;
        } else if (!strcmp(a, "--stop-on-exception")) {
            stop_on_exception = true;
        } else if (!strcmp(a, "--stop-on-fault")) {
            stop_on_fault = true;
        } else if (v && !strcmp(a, "--clock")) {
            if (!strcmp(v, "cycles")) {
                clock = GCRT_CLOCK_CYCLES;
            } else if (strcmp(v, "real")) {
                fprintf(stderr, "Unknown clock: %s (real or cycles)\n", v);
                return 1;
            }
            i++;
        } else if (!strcmp(a, "--profile")) {
            profile = true;
        } else if (!strcmp(a, "--host-profile")) {
            host_profile = true;
        } else if (v && !strcmp(a, "--profile-from")) {
            profile = true;
            profile_from = strtod(v, NULL), i++;
        } else if (v && !strcmp(a, "--profile-period")) {
            profile = true;
            profile_period = strtod(v, NULL), i++;
        } else if (!strcmp(a, "--console")) {
            gcrt_console_set_request(1);
        } else if (!strcmp(a, "--no-console")) {
            gcrt_console_set_request(0);
        } else if (v && !strcmp(a, "--backend")) {
            gcrt_backend_set_request(v), i++; /* checked when the window opens (unknown: auto, with a message) */
        } else if (!strcmp(a, "--hle-trace")) {
            hle_trace = true;
        } else if (v && !strcmp(a, "--hw-count")) {
            hw_count_path = v, i++;
        } else if (v && !strcmp(a, "--guest-log")) {
            guest_log_path = v, i++;
        } else if (v && !strcmp(a, "--rel-test")) {
            rel_test_path = v, i++;
        } else if (v && !strcmp(a, "--rel-test-warmup")) {
            rel_test_warmup = strtoull(v, NULL, 0), i++;
        } else if (v && !strcmp(a, "--dump")) {
            char* end = NULL;
            unsigned long address = strtoul(v, &end, 16);
            if (dump_count >= 8 || !end || *end != ':') {
                fprintf(stderr, "--dump expects HEX_ADDRESS:LENGTH (e.g. 80000C00:0x40), at most 8\n");
                return 1;
            }
            dump_address[dump_count] = (u32)address;
            dump_length[dump_count++] = (u32)strtoul(end + 1, NULL, 0);
            i++;
        } else if (v && !strcmp(a, "--disc")) {
            disc = v, i++;
        } else if (v && !strcmp(a, "--dol")) {
            dol_path = v, i++;
        } else if (v && !strcmp(a, "--max-blocks")) {
            limits.max_blocks = strtoull(v, NULL, 0), i++;
        } else if (v && !strcmp(a, "--max-seconds")) {
            limits.max_seconds = strtod(v, NULL), i++;
        } else if (v && !strcmp(a, "--trace")) {
            trace_path = v, i++;
        } else if (v && !strcmp(a, "--trace-limit")) {
            trace_limit = strtoull(v, NULL, 0), i++;
        } else if (v && !strcmp(a, "--io-log")) {
            io_log = (u32)strtoul(v, NULL, 0), i++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    /* Disc root: --disc, otherwise "<executable folder>/disc" when it holds sys/main.dol (installed copy), otherwise
     * the extracted disc of the build machine (development). */
    char default_disc[2048];
    if (disc) {
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "disc root: %s (--disc)", disc);
    } else {
        const char* why = gcrt_default_disc_root(info->disc_root, default_disc, sizeof(default_disc));
        disc = default_disc;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "disc root: %s (%s)", disc, why);
    }

    printf("%s (%s): %u recompiled modules, host RAM %u MiB\n", game && game->title ? game->title : info->game,
           info->version, gcrt_recompiled_modules.count, info->ram_size >> 20);

    char default_dol[1024];
    if (!dol_path) {
        join(default_dol, sizeof(default_dol), disc, "sys/main.dol");
        dol_path = default_dol;
    }
    size_t dol_size = 0;
    s_title_name = game && game->title ? game->title : info->game;
    u8* dol_data = read_file(dol_path, &dol_size);
    if (!dol_data) {
        fprintf(stderr, "gcrt: DOL not found: %s\n", dol_path);
        char shown[2048];
        gcrt_path_display(dol_path, shown, sizeof(shown));
        start_error("The game files were not found (DOL not found).\n\nThe game looked for:\n%s\n\n%s", shown,
                    k_reinstall);
    }
    u64 fnv = gcrt_fnv1a64(dol_data, dol_size);
    if (info->dol_fnv64 && fnv != info->dol_fnv64) {
        fprintf(stderr, "gcrt: this DOL (%016llX) is not the one that was recompiled (%016llX)\n",
                (unsigned long long)fnv, (unsigned long long)info->dol_fnv64);
        free(dol_data);
        /* Most likely the game files of another version (region) of the game: say which one if the header tells. */
        char path[1100], shown[2048], id[7] = "";
        bool magic = false;
        size_t size = 0;
        join(path, sizeof(path), disc, "sys/boot.bin");
        u8* boot = read_file(path, &size);
        if (boot && size >= 0x20)
            header_id(boot, id, &magic);
        free(boot);
        gcrt_path_display(dol_path, shown, sizeof(shown));
        if (id[0] && disc_id_valid(info->version) && strcmp(id, info->version))
            start_error("This executable is for %s but the game files are %s.\n\nGame files:\n%s\n\n%s", info->version,
                        id, shown, k_reinstall);
        start_error("The game files do not match this executable (built for %s).\n\nGame files:\n%s\n\n%s",
                    info->version, shown, k_reinstall);
    }
    /* Before anything else is set up (no window, no crash handler yet): the disc header and the disc image. */
    check_disc_header(disc, info->version);
    check_disc_image(disc, info->version);

    GcrtCpu* g = &g_cpu;
    if (!gcrt_cpu_init(g, info->ram_size, &gcrt_recompiled_modules)) {
        free(dol_data);
        return 1;
    }
    /* Crash report (host exception or abort): exception, host and guest stacks, minidump, guest memory
     * (platform/gcrt_crash.h). */
    gcrt_crash_handler_install(g);
    GcrtDolInfo dol;
    bool ok = gcrt_dol_load(&g->state, dol_data, dol_size, &dol);
    free(dol_data);
    if (!ok || !gcrt_boot_hle(g, disc, &dol)) {
        gcrt_cpu_free(g);
        return 1;
    }
    printf("DOL: entry %08X, loaded from %08X to %08X\n", dol.entry_point, dol.lowest, dol.highest);
    /* Hardware accesses of the boot set apart (none since milestone 10b: static boot, no more apploader):
     * the counter tracked by the milestone 10 criterion is the game's. */
    gcrt_hw_count_mark_boot(g);

    g->stop_on_unhandled_io = stop_on_io;
    g->stop_on_exception = stop_on_exception;
    g->stop_on_fault = stop_on_fault;
    g->profile = profile;
    g->prof_period_s = profile_period;
    g->prof_sample_from_ticks = (u64)(profile_from * (double)GCRT_TB_HZ);
    gcrt_cpu_set_clock(g, clock);
    g->io_log_limit = io_log;
    g->trace_limit = trace_limit;
    if (trace_path) {
        g->trace = gcrt_fopen_utf8(trace_path, "w");
        if (!g->trace)
            fprintf(stderr, "gcrt: cannot write the trace %s\n", trace_path);
    }
    /* HLE: SDK functions recognized by signature in the DOL code sections, replacements from the core,
     * then the game's own (install). */
    FILE* guest_log = guest_log_path ? gcrt_fopen_utf8(guest_log_path, "w") : NULL;
    if (guest_log_path && !guest_log)
        fprintf(stderr, "gcrt: cannot write %s\n", guest_log_path);
    gcrt_hle_set_guest_log(guest_log);
    gcrt_hle_init(g, hle_trace);
    gcrt_hle_scan_image(g, dol.text_address, dol.text_size, 7, "DOL");
    gcrt_hle_install_defaults(g);
    /* Native replacements of the SDK functions that touch the hardware (milestone 10: all native). */
    gcrt_native_install(g, disc);
    if (game && game->install)
        game->install(g);
    gcrt_hle_report(stderr);

    if (rel_test_path) {
        /* The test needs an initialized OS (native OS, module linking): we first run
         * the game's boot for a few blocks. */
        GcrtRunLimits warmup = {rel_test_warmup, 0.0};
        GcrtStopReason r = gcrt_cpu_run(g, &warmup);
        printf("Boot before the REL test: %s, %llu blocks\n", gcrt_stop_reason_name(r),
               (unsigned long long)g->blocks);
        int rc = gcrt_hle_rel_selftest(g, rel_test_path);
        fflush(stdout);
        if (guest_log)
            fclose(guest_log);
        gcrt_cpu_free(g);
        return rc;
    }

    /* Host side of the native SDK replacements: window and GX drawing (Aurora), inputs, audio output, screenshots and
     * run log (platform/gcrt_platform.h). No emulated hardware. */
    GcrtPlatform* platform = gcrt_platform_attach(g, game && game->title ? game->title : info->game, info->version, disc);
    u64 run_start = gcrt_prof_now();
    if (host_profile)
        gcrt_host_profiler_start();
    GcrtStopReason reason = gcrt_cpu_run(g, &limits);
    if (host_profile)
        gcrt_host_profiler_report(stdout);
    double run_seconds = (double)(gcrt_prof_now() - run_start) / gcrt_prof_ticks_per_second();
    gcrt_platform_detach(platform);
    printf("Stop: %s\n", gcrt_stop_reason_name(reason));
    gcrt_cpu_dump(g, stdout);
    gcrt_native_report(stdout);
    /* Hardware access counter: a stable line "HWCOUNT {json}" (format gcrt-hwcount/1, notes/runtime.md)
     * and, on request, the same object in a file. */
    printf("HWCOUNT ");
    gcrt_hw_count_json(g, stdout);
    printf("\n");
    if (hw_count_path && hw_count_path[0]) {
        FILE* f = gcrt_fopen_utf8(hw_count_path, "w");
        if (f) {
            gcrt_hw_count_json(g, f);
            fputc('\n', f);
            fclose(f);
        } else {
            fprintf(stderr, "gcrt: cannot write %s\n", hw_count_path);
        }
    }
    for (unsigned i = 0; i < dump_count; i++) {
        printf("memory %08X (%u bytes):\n", dump_address[i], dump_length[i]);
        gcrt_cpu_dump_memory(g, dump_address[i], dump_length[i], stdout);
    }
    if (hle_trace)
        gcrt_hle_report(stdout);
    if (profile)
        gcrt_cpu_profile_report(g, stdout, run_seconds, 0);
    fflush(stdout);
    if (guest_log)
        fclose(guest_log);
    gcrt_cpu_free(g);
    /* 0 = normal stop (limit reached, window closed or stop requested by the launcher);
     * 2 = abnormal stop (exception, unhandled access, game halted by OSPanic/PPCHalt...).
     * gcrt_platform_before_exit: stops and waits for the exit priority helper thread started by gcrt_platform_detach
     * (issue 35; the high priority class stays until the process ends), then writes the "exit: code" trace line. */
    return gcrt_platform_before_exit((reason == GCRT_STOP_BLOCK_LIMIT || reason == GCRT_STOP_TIME_LIMIT ||
                                      reason == GCRT_STOP_REQUESTED) ? 0 : 2);
}
