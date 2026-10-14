/* Native memory card: host-side model (see card_model.h).
 *
 * Behavior references (consulted only, nothing is copied): public decompilation of the SDK CARD library
 * (CARDCreate.c, CARDBlock.c, CARDOpen.c, CARDBios.c: external/cbr_decomp, CC0) cross-checked with the disassembly
 * of the game by the reverse agent (the internal SDK notes §§ 10-13); Dolphin's GCI format and GCI folders
 * (Source/Core/Core/HW/GCMemcard/GCMemcardDirectory.cpp, GCMemcardUtils.cpp, Common/NandPaths.cpp; GPL-2.0+). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/card_model.h"
#include "platform/gcrt_save_file.h"
#include "platform/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAT_FREE 0x0000u
#define FAT_LAST 0xFFFFu
/* __CARDPermMask of the SDK ([hypothesis]: value 0x1C from the reference decompilation, not re-read in this DOL): the
 * "global" (0x20) and "maker" (0x40) permissions therefore never grant access. */
#define PERM_MASK 0x1Cu
#define PERM_PUBLIC 0x04u
#define GCI_SUFFIX ".gci"
#define MAX_GCI_SIZE (GCRT_CARD_DIRENT_SIZE + (size_t)GCRT_CARD_MAX_BLOCKS * GCRT_CARD_SECTOR_SIZE)
#define MAX_CANDIDATES 1024u /* .gci files examined at most per mount */

static void clear_file(GcrtCardFile* f);

/* Empty card (formatted directory, empty FAT); folder and reports kept. */
static void reset_contents(GcrtCard* card) {
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++)
        clear_file(&card->files[i]);
    memset(card->fat, 0, sizeof(card->fat));
    card->free_blocks = (uint16_t)(card->cblock - GCRT_CARD_SYSTEM_BLOCKS);
    card->last_alloc = (uint16_t)(GCRT_CARD_SYSTEM_BLOCKS - 1u);
    card->loaded = false;
}

void gcrt_card_init(GcrtCard* card, unsigned size_mbit) {
    memset(card, 0, sizeof(*card));
    switch (size_mbit) {
    case 4: case 8: case 16: case 32: case 64: case 128:
        break;
    default:
        size_mbit = 16;
        break;
    }
    card->size_mbit = (uint16_t)size_mbit;
    card->cblock = (uint16_t)(size_mbit * 1024u * 1024u / 8u / GCRT_CARD_SECTOR_SIZE);
    reset_contents(card);
}

static void clear_file(GcrtCardFile* f) {
    free(f->data);
    memset(f, 0, sizeof(*f));
    memset(f->ent, 0xFF, sizeof(f->ent)); /* free entry, as in a formatted directory */
}

void gcrt_card_release(GcrtCard* card) { reset_contents(card); }

static uint16_t file_blocks(const GcrtCardFile* f) { return gcrt_card_be16(f->ent + GCRT_DIR_LENGTH); }
static uint32_t file_bytes(const GcrtCardFile* f) { return (uint32_t)file_blocks(f) * GCRT_CARD_SECTOR_SIZE; }

bool gcrt_card_valid_block(const GcrtCard* card, uint16_t block) {
    return block >= GCRT_CARD_SYSTEM_BLOCKS && block < card->cblock;
}

uint16_t gcrt_card_next_block(const GcrtCard* card, uint16_t block) {
    return block < GCRT_CARD_MAX_BLOCKS ? card->fat[block] : FAT_LAST;
}

/* __CARDAllocBlock: blocks taken starting from the last allocated one, wrapping around; returns the first one,
 * FAT_LAST if there is not enough room (checked beforehand by the caller). */
static uint16_t alloc_blocks(GcrtCard* card, uint16_t count) {
    if (card->free_blocks < count || count == 0)
        return FAT_LAST;
    uint16_t start = FAT_LAST, prev = FAT_LAST, block = card->last_alloc;
    unsigned seen = 0;
    while (count > 0) {
        if (++seen > (unsigned)(card->cblock - GCRT_CARD_SYSTEM_BLOCKS))
            return FAT_LAST; /* inconsistent FAT (BROKEN in the SDK) */
        block++;
        if (!gcrt_card_valid_block(card, block))
            block = (uint16_t)GCRT_CARD_SYSTEM_BLOCKS;
        if (card->fat[block] != FAT_FREE)
            continue;
        if (start == FAT_LAST)
            start = block;
        else
            card->fat[prev] = block;
        prev = block;
        card->fat[block] = FAT_LAST;
        count--;
        card->free_blocks--;
    }
    card->last_alloc = block;
    return start;
}

/* __CARDFreeBlock: frees the chain. */
static void free_blocks(GcrtCard* card, uint16_t block) {
    unsigned guard = 0;
    while (gcrt_card_valid_block(card, block) && guard++ < GCRT_CARD_MAX_BLOCKS) {
        const uint16_t next = card->fat[block];
        card->fat[block] = FAT_FREE;
        card->free_blocks++;
        block = next;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Names of the files on the PC (Dolphin's rule)
 * ------------------------------------------------------------------------------------------- */

static bool illegal_char(uint8_t c) {
    /* Common/NandPaths.cpp, IsIllegalCharacter; in addition, bytes >= 0x80 are escaped here (Dolphin first decodes
     * them as CP-1252): [hypothesis] no game affected, difference reported in notes/runtime.md. */
    return c <= 0x1F || c >= 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
           c == '?' || c == '\\' || c == '|';
}

void gcrt_card_host_name(const uint8_t ent[GCRT_CARD_DIRENT_SIZE], char* out, size_t size) {
    /* "<maker>-<game>-<name>" (each part cut at the first 0), escaped like EscapeFileName, then ".gci". */
    char raw[64];
    size_t n = 0;
    for (int i = 0; i < 2 && ent[GCRT_DIR_COMPANY + i]; i++)
        raw[n++] = (char)ent[GCRT_DIR_COMPANY + i];
    raw[n++] = '-';
    for (int i = 0; i < 4 && ent[GCRT_DIR_GAMENAME + i]; i++)
        raw[n++] = (char)ent[GCRT_DIR_GAMENAME + i];
    raw[n++] = '-';
    for (unsigned i = 0; i < GCRT_CARD_FILENAME_MAX && ent[GCRT_DIR_FILENAME + i]; i++)
        raw[n++] = (char)ent[GCRT_DIR_FILENAME + i];
    raw[n] = '\0';
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && used + 8 < size; i++) {
        const uint8_t c = (uint8_t)raw[i];
        if (c == '_' && i + 1 < n && raw[i + 1] == '_') {
            used += (size_t)snprintf(out + used, size - used, "__5f____5f__");
            i++;
        } else if (illegal_char(c)) {
            used += (size_t)snprintf(out + used, size - used, "__%02x__", c);
        } else {
            out[used++] = (char)c;
            out[used] = '\0';
        }
    }
    snprintf(out + used, size - used, "%s", GCI_SUFFIX);
}

static void host_path(const GcrtCard* card, const char* name, char* out, size_t size) {
    snprintf(out, size, "%s%s", card->dir, name);
}

/* Writes the entry and the data into the file on the PC (safe write). */
static bool persist(GcrtCard* card, const uint8_t ent[GCRT_CARD_DIRENT_SIZE], const uint8_t* data, uint32_t bytes,
                    const char* host_name) {
    const size_t total = GCRT_CARD_DIRENT_SIZE + (size_t)bytes;
    uint8_t* image = (uint8_t*)malloc(total);
    if (!image)
        return false;
    memcpy(image, ent, GCRT_CARD_DIRENT_SIZE);
    if (bytes)
        memcpy(image + GCRT_CARD_DIRENT_SIZE, data, bytes);
    char path[1400];
    host_path(card, host_name, path, sizeof(path));
    const bool ok = gcrt_sf_write_atomic(path, image, total);
    free(image);
    if (!ok)
        card->stats.io_errors++;
    return ok;
}

/* ---------------------------------------------------------------------------------------------
 * Insertion: reading the folder
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    char name[256];
    uint8_t* image; /* whole file */
    size_t size;
    bool current_game;
    bool taken;
} Candidate;

typedef struct {
    GcrtCard* card;
    Candidate* list;
    unsigned count, cap;
    const uint8_t* game;
    const uint8_t* company;
} ScanCtx;

static bool same_identity(const uint8_t* a, const uint8_t* b) {
    /* Same game, same maker, same name (Dolphin's HasSameIdentity / __CARDCompareFileName). */
    return !memcmp(a + GCRT_DIR_GAMENAME, b + GCRT_DIR_GAMENAME, 4) &&
           !memcmp(a + GCRT_DIR_COMPANY, b + GCRT_DIR_COMPANY, 2) &&
           !strncmp((const char*)a + GCRT_DIR_FILENAME, (const char*)b + GCRT_DIR_FILENAME, GCRT_CARD_FILENAME_MAX);
}

static void scan_visit(const char* name, uint64_t size, void* user) {
    ScanCtx* c = (ScanCtx*)user;
    GcrtCard* card = c->card;
    if (strlen(name) >= sizeof(c->list[0].name) || c->count >= MAX_CANDIDATES) {
        card->stats.ignored++;
        return;
    }
    if (size < GCRT_CARD_DIRENT_SIZE + GCRT_CARD_SECTOR_SIZE || size > MAX_GCI_SIZE) {
        card->stats.ignored++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: %s ignored (size %llu invalid for a GCI file)", name,
                          (unsigned long long)size);
        return;
    }
    char path[1400];
    host_path(card, name, path, sizeof(path));
    uint8_t* image = NULL;
    size_t n = 0;
    if (!gcrt_sf_read_all(path, &image, &n, MAX_GCI_SIZE)) {
        card->stats.ignored++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: %s ignored (unreadable)", name);
        return;
    }
    const uint16_t blocks = gcrt_card_be16(image + GCRT_DIR_LENGTH);
    const bool header_ok = image[GCRT_DIR_GAMENAME] != 0xFF && image[GCRT_DIR_FILENAME] != 0 &&
                           image[GCRT_DIR_FILENAME] != 0xFF && blocks > 0;
    if (!header_ok || n != GCRT_CARD_DIRENT_SIZE + (size_t)blocks * GCRT_CARD_SECTOR_SIZE) {
        card->stats.ignored++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: %s ignored (invalid GCI header or size different from "
                                               "%u blocks)", name, blocks);
        free(image);
        return;
    }
    for (unsigned i = 0; i < c->count; i++) {
        if (same_identity(c->list[i].image, image)) {
            card->stats.ignored++;
            gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: %s ignored (same card file as %s)", name,
                              c->list[i].name);
            free(image);
            return;
        }
    }
    if (c->count == c->cap) {
        const unsigned cap = c->cap ? c->cap * 2 : 32;
        Candidate* grown = (Candidate*)realloc(c->list, cap * sizeof(Candidate));
        if (!grown) {
            free(image);
            return;
        }
        c->list = grown;
        c->cap = cap;
    }
    Candidate* cand = &c->list[c->count++];
    memset(cand, 0, sizeof(*cand));
    snprintf(cand->name, sizeof(cand->name), "%s", name);
    cand->image = image;
    cand->size = n;
    cand->current_game = !memcmp(image + GCRT_DIR_GAMENAME, c->game, 4) &&
                         !memcmp(image + GCRT_DIR_COMPANY, c->company, 2);
}

static int free_entries(const GcrtCard* card) {
    int n = 0;
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++)
        n += !card->files[i].used;
    return n;
}

int gcrt_card_load(GcrtCard* card, const char* dir, const uint8_t game[4], const uint8_t company[2]) {
    /* Entry numbers of the previous mount, by name of the file on the PC. */
    char previous[GCRT_CARD_MAX_FILE][256];
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++)
        snprintf(previous[i], sizeof(previous[i]), "%s", card->files[i].used ? card->files[i].host_name : "");
    reset_contents(card);
    card->stats.loads++;

    const size_t dl = strlen(dir);
    const bool sep = dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/');
    snprintf(card->dir, sizeof(card->dir), "%s%s", dir, sep ? "" : "/");
    if (!gcrt_sf_mkdirs(card->dir)) {
        card->stats.io_errors++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: folder %s unusable", card->dir);
        return GCRT_CARD_IOERROR;
    }
    card->stats.orphans_removed += (unsigned)gcrt_sf_clean_orphans(card->dir, GCI_SUFFIX);

    ScanCtx ctx = {card, NULL, 0, 0, game, company};
    if (gcrt_sf_list(card->dir, GCI_SUFFIX, scan_visit, &ctx) < 0) {
        card->stats.io_errors++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: folder %s unreadable", card->dir);
        free(ctx.list);
        return GCRT_CARD_IOERROR;
    }

    /* Selection: current game first, then the other games with a reserve (Dolphin's rule). */
    const unsigned data_blocks = (unsigned)(card->cblock - GCRT_CARD_SYSTEM_BLOCKS);
    unsigned used_blocks = 0, used_entries = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < ctx.count; i++) {
            Candidate* cand = &ctx.list[i];
            if (cand->current_game != (pass == 0))
                continue;
            const unsigned blocks = gcrt_card_be16(cand->image + GCRT_DIR_LENGTH);
            bool fits = used_entries < GCRT_CARD_MAX_FILE && used_blocks + blocks <= data_blocks;
            if (pass == 1)
                fits = fits && used_entries < GCRT_CARD_MAX_FILE - 15 &&
                       data_blocks - used_blocks - blocks >= data_blocks / 10;
            if (!fits) {
                card->stats.ignored++;
                gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: %s not loaded (%u-block card full)", cand->name,
                                  data_blocks);
                continue;
            }
            cand->taken = true;
            used_blocks += blocks;
            used_entries++;
        }
    }

    /* Entry numbers: the same as in the previous mount if the file was there, otherwise the first free entry. */
    int slot_of[MAX_CANDIDATES];
    for (unsigned i = 0; i < ctx.count ; i++) {
        slot_of[i] = -1;
        if (!ctx.list[i].taken)
            continue;
        for (int s = 0; s < GCRT_CARD_MAX_FILE; s++) {
            if (previous[s][0] && !strcmp(previous[s], ctx.list[i].name) && !card->files[s].used) {
                slot_of[i] = s;
                card->files[s].used = true; /* reserved */
                break;
            }
        }
    }
    for (unsigned i = 0; i < ctx.count ; i++) {
        if (!ctx.list[i].taken || slot_of[i] >= 0)
            continue;
        for (int s = 0; s < GCRT_CARD_MAX_FILE; s++) {
            if (!card->files[s].used) {
                slot_of[i] = s;
                card->files[s].used = true;
                break;
            }
        }
    }
    /* Setup, blocks allocated in the order of the entries (like a card filled in that order). */
    for (int s = 0; s < GCRT_CARD_MAX_FILE; s++)
        card->files[s].used = false;
    unsigned loaded = 0;
    for (int s = 0; s < GCRT_CARD_MAX_FILE; s++) {
        for (unsigned i = 0; i < ctx.count ; i++) {
            if (slot_of[i] != s)
                continue;
            Candidate* cand = &ctx.list[i];
            GcrtCardFile* f = &card->files[s];
            memcpy(f->ent, cand->image, GCRT_CARD_DIRENT_SIZE);
            const uint32_t bytes = file_bytes(f);
            f->data = (uint8_t*)malloc(bytes);
            if (!f->data) {
                clear_file(f);
                break;
            }
            memcpy(f->data, cand->image + GCRT_CARD_DIRENT_SIZE, bytes);
            const uint16_t start = alloc_blocks(card, file_blocks(f));
            gcrt_card_put16(f->ent + GCRT_DIR_START_BLOCK, start);
            snprintf(f->host_name, sizeof(f->host_name), "%s", cand->name);
            f->used = true;
            loaded++;
            break;
        }
    }
    for (unsigned i = 0; i < ctx.count; i++)
        free(ctx.list[i].image);
    free(ctx.list);
    card->loaded = true;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: %s: %u files loaded, %u free blocks out of %u, %d free "
                                           "entries", card->dir, loaded, card->free_blocks, data_blocks,
                      free_entries(card));
    return GCRT_CARD_READY;
}

/* ---------------------------------------------------------------------------------------------
 * Access and search
 * ------------------------------------------------------------------------------------------- */

GcrtCardFile* gcrt_card_file(GcrtCard* card, int file_no) {
    if (file_no < 0 || file_no >= GCRT_CARD_MAX_FILE)
        return NULL;
    return &card->files[file_no];
}

int gcrt_card_access(const GcrtCardFile* f, const uint8_t game[4], const uint8_t company[2]) {
    if (!f || !f->used || f->ent[GCRT_DIR_GAMENAME] == 0xFF)
        return GCRT_CARD_NOFILE;
    if (!memcmp(f->ent + GCRT_DIR_GAMENAME, game, 4) && !memcmp(f->ent + GCRT_DIR_COMPANY, company, 2))
        return GCRT_CARD_READY;
    return GCRT_CARD_NOPERM;
}

int gcrt_card_writable(const GcrtCardFile* f, const uint8_t game[4], const uint8_t company[2]) {
    int r = gcrt_card_access(f, game, company);
    if (r == GCRT_CARD_NOPERM) {
        static const uint8_t none[6] = {0};
        const uint8_t perm = (uint8_t)(f->ent[GCRT_DIR_PERMISSION] & PERM_MASK);
        if ((perm & 0x20) && !memcmp(f->ent + GCRT_DIR_GAMENAME, none, 4) && !memcmp(f->ent + GCRT_DIR_COMPANY, none, 2))
            return GCRT_CARD_READY;
        if ((perm & 0x40) && !memcmp(f->ent + GCRT_DIR_GAMENAME, none, 4) && !memcmp(f->ent + GCRT_DIR_COMPANY, company, 2))
            return GCRT_CARD_READY;
    }
    return r;
}

int gcrt_card_readable(const GcrtCardFile* f, const uint8_t game[4], const uint8_t company[2]) {
    int r = gcrt_card_writable(f, game, company);
    if (r == GCRT_CARD_NOPERM && (f->ent[GCRT_DIR_PERMISSION] & PERM_PUBLIC))
        return GCRT_CARD_READY;
    return r;
}

/* __CARDCompareFileName: up to 32 bytes, the searched name may end earlier. */
static bool compare_name(const uint8_t* ent_name, const char* name) {
    for (unsigned i = 0; i < GCRT_CARD_FILENAME_MAX; i++) {
        const char c1 = (char)ent_name[i], c2 = name[i];
        if (c1 != c2)
            return false;
        if (c2 == '\0')
            return true;
    }
    return name[GCRT_CARD_FILENAME_MAX] == '\0';
}

int gcrt_card_find(GcrtCard* card, const char* name, const uint8_t game[4], const uint8_t company[2], int* file_no) {
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++) {
        const GcrtCardFile* f = &card->files[i];
        if (gcrt_card_access(f, game, company) < 0)
            continue;
        if (compare_name(f->ent + GCRT_DIR_FILENAME, name)) {
            *file_no = i;
            return GCRT_CARD_READY;
        }
    }
    return GCRT_CARD_NOFILE;
}

/* ---------------------------------------------------------------------------------------------
 * Operations
 * ------------------------------------------------------------------------------------------- */

/* Free PC name for a new entry: Dolphin's one, or followed by "~N" if a file that is not on the card (invalid
 * file, left as is) already takes it. */
static bool pick_host_name(GcrtCard* card, const uint8_t ent[GCRT_CARD_DIRENT_SIZE], char* out, size_t size) {
    char base[256];
    gcrt_card_host_name(ent, base, sizeof(base));
    for (int k = 0; k < 10; k++) {
        if (k == 0) {
            snprintf(out, size, "%s", base);
        } else {
            const size_t bl = strlen(base);
            snprintf(out, size, "%.*s~%d%s", (int)(bl - strlen(GCI_SUFFIX)), base, k, GCI_SUFFIX);
        }
        char path[1400];
        host_path(card, out, path, sizeof(path));
        bool taken = gcrt_sf_is_file(path);
        for (int i = 0; i < GCRT_CARD_MAX_FILE && !taken; i++)
            taken = card->files[i].used && !strcmp(card->files[i].host_name, out);
        if (!taken)
            return true;
    }
    return false;
}

int gcrt_card_create(GcrtCard* card, const char* name, uint32_t size, const uint8_t game[4], const uint8_t company[2],
                     uint32_t time, int* file_no) {
    int free_no = -1;
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++) {
        const GcrtCardFile* f = &card->files[i];
        if (!f->used || f->ent[GCRT_DIR_GAMENAME] == 0xFF) {
            if (free_no < 0)
                free_no = i;
        } else if (!memcmp(f->ent + GCRT_DIR_GAMENAME, game, 4) && !memcmp(f->ent + GCRT_DIR_COMPANY, company, 2) &&
                   compare_name(f->ent + GCRT_DIR_FILENAME, name)) {
            return GCRT_CARD_EXIST;
        }
    }
    if (free_no < 0)
        return GCRT_CARD_NOENT;
    if ((uint64_t)GCRT_CARD_SECTOR_SIZE * card->free_blocks < size)
        return GCRT_CARD_INSSPACE;

    GcrtCardFile* f = &card->files[free_no];
    uint8_t ent[GCRT_CARD_DIRENT_SIZE];
    memset(ent, 0xFF, sizeof(ent));
    const uint16_t blocks = (uint16_t)(size / GCRT_CARD_SECTOR_SIZE);
    gcrt_card_put16(ent + GCRT_DIR_LENGTH, blocks);
    /* strncpy of the SDK: the name padded with zeros up to 32 bytes. */
    const size_t nl = strlen(name);
    for (unsigned i = 0; i < GCRT_CARD_FILENAME_MAX; i++)
        ent[GCRT_DIR_FILENAME + i] = i < nl ? (uint8_t)name[i] : 0;
    memcpy(ent + GCRT_DIR_GAMENAME, game, 4);
    memcpy(ent + GCRT_DIR_COMPANY, company, 2);
    ent[GCRT_DIR_PERMISSION] = PERM_PUBLIC; /* CARD_ATTR_PUBLIC */
    ent[GCRT_DIR_COPY_TIMES] = 0;
    ent[GCRT_DIR_BANNER_FORMAT] = 0;
    gcrt_card_put32(ent + GCRT_DIR_ICON_ADDR, 0xFFFFFFFFu);
    gcrt_card_put16(ent + GCRT_DIR_ICON_FORMAT, 0);
    gcrt_card_put16(ent + GCRT_DIR_ICON_SPEED, 1); /* CARDSetIconSpeed(ent, 0, CARD_STAT_SPEED_FAST) */
    gcrt_card_put32(ent + GCRT_DIR_COMMENT_ADDR, 0xFFFFFFFFu);
    gcrt_card_put32(ent + GCRT_DIR_TIME, time);

    char host[256];
    if (!pick_host_name(card, ent, host, sizeof(host))) {
        card->stats.io_errors++;
        return GCRT_CARD_IOERROR;
    }
    uint8_t* data = (uint8_t*)calloc(size ? size : 1, 1);
    if (!data)
        return GCRT_CARD_IOERROR;
    const uint16_t saved_last = card->last_alloc;
    const uint16_t start = alloc_blocks(card, blocks);
    if (start == FAT_LAST) {
        free(data);
        return GCRT_CARD_BROKEN;
    }
    gcrt_card_put16(ent + GCRT_DIR_START_BLOCK, start);
    if (!persist(card, ent, data, size, host)) {
        free_blocks(card, start);
        card->last_alloc = saved_last;
        free(data);
        return GCRT_CARD_IOERROR;
    }
    clear_file(f);
    memcpy(f->ent, ent, sizeof(ent));
    f->data = data;
    snprintf(f->host_name, sizeof(f->host_name), "%s", host);
    f->used = true;
    card->stats.creates++;
    *file_no = free_no;
    return GCRT_CARD_READY;
}

int gcrt_card_read(GcrtCard* card, int file_no, uint32_t offset, void* dst, uint32_t length) {
    GcrtCardFile* f = gcrt_card_file(card, file_no);
    if (!f || !f->used)
        return GCRT_CARD_NOFILE;
    if ((uint64_t)offset + length > file_bytes(f))
        return GCRT_CARD_LIMIT;
    memcpy(dst, f->data + offset, length);
    return GCRT_CARD_READY;
}

int gcrt_card_write(GcrtCard* card, int file_no, uint32_t offset, const void* src, uint32_t length, uint32_t time) {
    GcrtCardFile* f = gcrt_card_file(card, file_no);
    if (!f || !f->used)
        return GCRT_CARD_NOFILE;
    const uint32_t bytes = file_bytes(f);
    if ((uint64_t)offset + length > bytes)
        return GCRT_CARD_LIMIT;
    uint8_t* data = (uint8_t*)malloc(bytes);
    if (!data)
        return GCRT_CARD_IOERROR;
    memcpy(data, f->data, bytes);
    memcpy(data + offset, src, length);
    uint8_t ent[GCRT_CARD_DIRENT_SIZE];
    memcpy(ent, f->ent, sizeof(ent));
    gcrt_card_put32(ent + GCRT_DIR_TIME, time);
    if (!persist(card, ent, data, bytes, f->host_name)) {
        free(data);
        return GCRT_CARD_IOERROR;
    }
    free(f->data);
    f->data = data;
    memcpy(f->ent, ent, sizeof(ent));
    card->stats.writes++;
    return GCRT_CARD_READY;
}

int gcrt_card_set_entry(GcrtCard* card, int file_no, const uint8_t ent[GCRT_CARD_DIRENT_SIZE]) {
    GcrtCardFile* f = gcrt_card_file(card, file_no);
    if (!f || !f->used)
        return GCRT_CARD_NOFILE;
    if (!persist(card, ent, f->data, file_bytes(f), f->host_name))
        return GCRT_CARD_IOERROR;
    memcpy(f->ent, ent, GCRT_CARD_DIRENT_SIZE);
    card->stats.status_writes++;
    return GCRT_CARD_READY;
}

int gcrt_card_delete(GcrtCard* card, int file_no) {
    GcrtCardFile* f = gcrt_card_file(card, file_no);
    if (!f || !f->used)
        return GCRT_CARD_NOFILE;
    char path[1400];
    host_path(card, f->host_name, path, sizeof(path));
    if (!gcrt_sf_remove(path)) {
        card->stats.io_errors++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: cannot delete %s", path);
        return GCRT_CARD_IOERROR;
    }
    free_blocks(card, gcrt_card_be16(f->ent + GCRT_DIR_START_BLOCK));
    clear_file(f);
    card->stats.deletes++;
    return GCRT_CARD_READY;
}

void gcrt_card_free_space(const GcrtCard* card, int32_t* bytes, int32_t* files) {
    if (bytes)
        *bytes = (int32_t)(GCRT_CARD_SECTOR_SIZE * card->free_blocks);
    if (files) {
        /* CARDFreeBlocks counts the entries whose first name byte is 0xFF. */
        int32_t n = 0;
        for (int i = 0; i < GCRT_CARD_MAX_FILE; i++)
            n += card->files[i].ent[GCRT_DIR_FILENAME] == 0xFF;
        *files = n;
    }
}

int gcrt_card_format(GcrtCard* card) {
    char sub[128];
    const time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    snprintf(sub, sizeof(sub), "format-backup-%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    char backup[1400];
    host_path(card, sub, backup, sizeof(backup));
    bool any = false;
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++)
        any |= card->files[i].used;
    if (any && !gcrt_sf_mkdirs(backup)) {
        card->stats.io_errors++;
        return GCRT_CARD_IOERROR;
    }
    for (int i = 0; i < GCRT_CARD_MAX_FILE; i++) {
        GcrtCardFile* f = &card->files[i];
        if (!f->used)
            continue;
        char from[1400], to[1600];
        host_path(card, f->host_name, from, sizeof(from));
        snprintf(to, sizeof(to), "%s/%s", backup, f->host_name);
        if (!gcrt_sf_move(from, to)) {
            card->stats.io_errors++;
            gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: format: %s could not be moved to %s", from,
                              backup);
            return GCRT_CARD_IOERROR;
        }
        clear_file(f);
    }
    reset_contents(card);
    card->loaded = true;
    card->stats.formats++;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "card: formatted (old files moved to %s)", backup);
    return GCRT_CARD_READY;
}
