/* Native memory card (milestone 7): model of a GameCube card that is always inserted and formatted, whose content
 * is a folder on the PC with one .gci file per file of the card (Dolphin's GCI format: the 0x40-byte directory
 * entry, as it is on the card, followed by the data blocks).
 *
 * This module is host-side: it does not touch game memory (the link with the SDK CARD API is in hle/native_card.c)
 * and can be tested on its own (tests/card/). It reproduces what the SDK CARD library does with the directory and
 * the allocation table (FAT) of a card (creation, writing, deletion, status, free blocks), without the card itself:
 * directory of 127 entries, 8 KiB blocks, 5 system blocks, block allocation like __CARDAllocBlock. Each change to a
 * file is written to disk immediately with a safe write (platform/gcrt_save_file.h): the .gci file is replaced as a
 * whole, never modified in place.
 *
 * Generic: no reference to a game. Names of the files on the PC: the ones Dolphin gives in its GCI folders
 * ("<maker>-<game>-<name>.gci", forbidden characters escaped as "__xx__"), for exchange with Dolphin. */
#ifndef GCRT_CARD_MODEL_H
#define GCRT_CARD_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Card constants (SDK, card.h). */
#define GCRT_CARD_SECTOR_SIZE 0x2000u
#define GCRT_CARD_MAX_FILE 127
#define GCRT_CARD_SYSTEM_BLOCKS 5u
#define GCRT_CARD_DIRENT_SIZE 0x40u
#define GCRT_CARD_FILENAME_MAX 32u
#define GCRT_CARD_MAX_BLOCKS 2048u /* 128 Mbit card */

/* CARDDir directory entry (big-endian, 0x40 bytes): offsets. */
#define GCRT_DIR_GAMENAME 0x00u
#define GCRT_DIR_COMPANY 0x04u
#define GCRT_DIR_BANNER_FORMAT 0x07u
#define GCRT_DIR_FILENAME 0x08u
#define GCRT_DIR_TIME 0x28u
#define GCRT_DIR_ICON_ADDR 0x2Cu
#define GCRT_DIR_ICON_FORMAT 0x30u
#define GCRT_DIR_ICON_SPEED 0x32u
#define GCRT_DIR_PERMISSION 0x34u
#define GCRT_DIR_COPY_TIMES 0x35u
#define GCRT_DIR_START_BLOCK 0x36u
#define GCRT_DIR_LENGTH 0x38u
#define GCRT_DIR_COMMENT_ADDR 0x3Cu

/* Result codes (SDK CARD_RESULT_*). */
enum {
    GCRT_CARD_READY = 0,
    GCRT_CARD_BUSY = -1,
    GCRT_CARD_WRONGDEVICE = -2,
    GCRT_CARD_NOCARD = -3,
    GCRT_CARD_NOFILE = -4,
    GCRT_CARD_IOERROR = -5,
    GCRT_CARD_BROKEN = -6,
    GCRT_CARD_EXIST = -7,
    GCRT_CARD_NOENT = -8,
    GCRT_CARD_INSSPACE = -9,
    GCRT_CARD_NOPERM = -10,
    GCRT_CARD_LIMIT = -11,
    GCRT_CARD_NAMETOOLONG = -12,
    GCRT_CARD_ENCODING = -13,
    GCRT_CARD_CANCELED = -14,
    GCRT_CARD_FATAL_ERROR = -128,
};

typedef struct GcrtCardFile {
    bool used;
    uint8_t ent[GCRT_CARD_DIRENT_SIZE]; /* directory entry = header of the .gci file */
    uint8_t* data;                      /* length (blocks) × 8 KiB */
    char host_name[256];                /* name of the .gci file in the card folder */
} GcrtCardFile;

/* Report (accumulated over all mounts). */
typedef struct GcrtCardStats {
    unsigned loads, ignored, orphans_removed;
    unsigned creates, writes, deletes, status_writes, formats, io_errors;
} GcrtCardStats;

typedef struct GcrtCard {
    char dir[1024];      /* card folder (UTF-8, ending with a separator) */
    uint16_t size_mbit;  /* 4, 8, 16, 32, 64 or 128 */
    uint16_t cblock;     /* blocks of the card, system ones included (size_mbit × 16) */
    uint16_t fat[GCRT_CARD_MAX_BLOCKS];
    uint16_t free_blocks;
    uint16_t last_alloc;
    GcrtCardFile files[GCRT_CARD_MAX_FILE];
    bool loaded;
    GcrtCardStats stats;
} GcrtCard;

/* Prepares an empty card of `size_mbit` megabits (4 to 128, power of 2; otherwise 16). */
void gcrt_card_init(GcrtCard* card, unsigned size_mbit);
/* Frees the data in memory. */
void gcrt_card_release(GcrtCard* card);

/* "Insertion": reads the folder `dir` (created if needed), deletes orphan temporary files, loads the valid .gci
 * files. Those of the current game (`game`, `company`: disc identifier) first, then those of other games as long as
 * at least 10 % of the blocks and 15 entries remain free (same rule as Dolphin's GCI folders). A file that is already
 * loaded keeps its entry number (fileNo) from one mount to the next. Returns READY, or IOERROR if the folder is
 * unusable (the card is then empty). */
int gcrt_card_load(GcrtCard* card, const char* dir, const uint8_t game[4], const uint8_t company[2]);

/* File by entry number: `access` = gcrt_card_access applied. */
GcrtCardFile* gcrt_card_file(GcrtCard* card, int file_no);
/* Access right of an entry for disc `game`/`company` (__CARDAccess): NOFILE (free entry), READY or NOPERM. */
int gcrt_card_access(const GcrtCardFile* f, const uint8_t game[4], const uint8_t company[2]);
/* Reading (__CARDIsReadable) and writing (__CARDIsWritable): NOPERM raised by the permissions of the entry. */
int gcrt_card_readable(const GcrtCardFile* f, const uint8_t game[4], const uint8_t company[2]);
int gcrt_card_writable(const GcrtCardFile* f, const uint8_t game[4], const uint8_t company[2]);
/* Search by name among the accessible entries (__CARDGetFileNo): READY and *file_no, or NOFILE. */
int gcrt_card_find(GcrtCard* card, const char* name, const uint8_t game[4], const uint8_t company[2], int* file_no);

/* Creation (CARDCreateAsync + CreateCallbackFat): `size` multiple of 8 KiB, name of at most 32 bytes (checked by the
 * caller). EXIST, NOENT (directory full), INSSPACE, IOERROR (file on the PC cannot be written: nothing is created) or
 * READY and *file_no. `time`: seconds since January 1, 2000 (game clock). */
int gcrt_card_create(GcrtCard* card, const char* name, uint32_t size, const uint8_t game[4], const uint8_t company[2],
                     uint32_t time, int* file_no);
/* Reads `length` bytes at `offset` in the data of the file (bounds checked by the caller). */
int gcrt_card_read(GcrtCard* card, int file_no, uint32_t offset, void* dst, uint32_t length);
/* Write then update of the entry time (WriteCallback), written to disk immediately. IOERROR: nothing has changed,
 * neither in memory nor on disk. */
int gcrt_card_write(GcrtCard* card, int file_no, uint32_t offset, const void* src, uint32_t length, uint32_t time);
/* Fields of CARDSetStatus (entry already updated by the caller in `ent`): replaces the entry and writes it. */
int gcrt_card_set_entry(GcrtCard* card, int file_no, const uint8_t ent[GCRT_CARD_DIRENT_SIZE]);
/* Deletion (CARDDeleteAsync / CARDFastDeleteAsync): file on the PC deleted, blocks freed. */
int gcrt_card_delete(GcrtCard* card, int file_no);
/* Free bytes (sector × free blocks) and free entries (CARDFreeBlocks). */
void gcrt_card_free_space(const GcrtCard* card, int32_t* bytes, int32_t* files);
/* Formatting (CARDFormat): the card becomes empty. The .gci files are not erased but moved into a subfolder
 * "format-backup-YYYYMMDD-HHMMSS" of the card folder. */
int gcrt_card_format(GcrtCard* card);
/* Next block of a file in the FAT (0xFFFF: last). */
uint16_t gcrt_card_next_block(const GcrtCard* card, uint16_t block);
/* Is the block number a data block (CARDIsValidBlockNo)? */
bool gcrt_card_valid_block(const GcrtCard* card, uint16_t block);

/* Name of the file on the PC for an entry (Dolphin's rule), in `out`. */
void gcrt_card_host_name(const uint8_t ent[GCRT_CARD_DIRENT_SIZE], char* out, size_t size);

/* Big-endian accessors for the entries. */
static inline uint16_t gcrt_card_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t gcrt_card_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void gcrt_card_put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void gcrt_card_put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#ifdef __cplusplus
}
#endif

#endif /* GCRT_CARD_MODEL_H */
