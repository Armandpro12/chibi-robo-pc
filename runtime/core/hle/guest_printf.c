/* printf formatting with guest arguments (PowerPC, CodeWarrior EABI/SVR4 ABI).
 *
 * Variadic arguments: integers in r3...r10, floating-point values (always promoted to double) in f1...f8; the
 * caller sets CR bit 6 to 1 if it passed floating-point values in registers (the callee saves them only in that
 * case). Beyond 8, the arguments are in the caller's parameter area, at r1 + 8 on entry (4-byte words; double and
 * long long aligned on 8). A long long takes a pair of registers starting at an even index (r3/r4, r5/r6...).
 * va_list: struct { u8 gpr; u8 fpr; u16 pad; u32 overflow_arg_area; u32 reg_save_area; }, save area
 * = 8 GPRs (32 bytes) then 8 FPRs (64 bytes). [verified: prologue of the game's OSReport (saves r3...r10 at sp+8,
 * f1...f8 at sp+40 if CR6); rules match the "System V ABI PowerPC Processor Supplement" (1995)] */
#include "hle/gcrt_hle.h"

#include <string.h>

static bool in_ram(CPUState* cpu, u32 address, u32 size) {
    if (address < GC_RAM_BASE)
        return false;
    u32 offset = address - GC_RAM_BASE;
    return offset < cpu->ram_size && size <= cpu->ram_size - offset;
}

bool gcrt_guest_string(GcrtCpu* g, u32 address, char* out, size_t out_size) {
    CPUState* cpu = &g->state;
    if (!out_size)
        return false;
    out[0] = 0;
    if (!in_ram(cpu, address, 1))
        return false;
    size_t i = 0;
    for (; i + 1 < out_size && in_ram(cpu, address + (u32)i, 1); i++) {
        char c = (char)cpu->ram[address - GC_RAM_BASE + i];
        if (!c)
            break;
        out[i] = c;
    }
    out[i] = 0;
    return true;
}

bool gcrt_guest_plausible_text(GcrtCpu* g, u32 address) {
    CPUState* cpu = &g->state;
    for (u32 i = 0; i < 1024u; i++) {
        if (!in_ram(cpu, address + i, 1))
            return false;
        u8 c = cpu->ram[address - GC_RAM_BASE + i];
        if (c == 0)
            return i > 0;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
            return false;
    }
    return false;
}

void gcrt_guest_args_from_registers(GcrtGuestArgs* a, GcrtCpu* g, u32 fixed_gprs) {
    memset(a, 0, sizeof(*a));
    a->g = g;
    a->from_registers = true;
    a->gpr = fixed_gprs;
    a->overflow = g->state.gpr[1] + 8u;
}

void gcrt_guest_args_from_va_list(GcrtGuestArgs* a, GcrtCpu* g, u32 va_list_address) {
    CPUState* cpu = &g->state;
    memset(a, 0, sizeof(*a));
    a->g = g;
    a->gpr = mem_read8(cpu, va_list_address);
    a->fpr = mem_read8(cpu, va_list_address + 1u);
    a->overflow = mem_read32(cpu, va_list_address + 4u);
    a->reg_save = mem_read32(cpu, va_list_address + 8u);
}

static u32 next_u32(GcrtGuestArgs* a) {
    CPUState* cpu = &a->g->state;
    if (a->gpr < 8u) {
        u32 v = a->from_registers ? cpu->gpr[3u + a->gpr] : mem_read32(cpu, a->reg_save + 4u * a->gpr);
        a->gpr++;
        return v;
    }
    u32 v = mem_read32(cpu, a->overflow);
    a->overflow += 4u;
    return v;
}

static u64 next_u64(GcrtGuestArgs* a) {
    CPUState* cpu = &a->g->state;
    a->gpr += a->gpr & 1u;
    if (a->gpr < 7u) {
        u32 hi = a->from_registers ? cpu->gpr[3u + a->gpr] : mem_read32(cpu, a->reg_save + 4u * a->gpr);
        u32 lo = a->from_registers ? cpu->gpr[4u + a->gpr] : mem_read32(cpu, a->reg_save + 4u * (a->gpr + 1u));
        a->gpr += 2u;
        return ((u64)hi << 32) | lo;
    }
    a->gpr = 8u;
    a->overflow = (a->overflow + 7u) & ~7u;
    u64 v = mem_read64(cpu, a->overflow);
    a->overflow += 8u;
    return v;
}

static double next_double(GcrtGuestArgs* a) {
    CPUState* cpu = &a->g->state;
    u64 bits;
    if (a->fpr < 8u) {
        if (a->from_registers) {
            double d = cpu->fpr[1u + a->fpr];
            a->fpr++;
            return d;
        }
        bits = mem_read64(cpu, a->reg_save + 32u + 8u * a->fpr);
        a->fpr++;
    } else {
        a->overflow = (a->overflow + 7u) & ~7u;
        bits = mem_read64(cpu, a->overflow);
        a->overflow += 8u;
    }
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static void append(char* out, size_t out_size, size_t* len, const char* s, size_t n) {
    if (*len + 1 >= out_size)
        return;
    size_t room = out_size - 1 - *len;
    if (n > room)
        n = room;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = 0;
}

size_t gcrt_guest_vformat(GcrtGuestArgs* a, u32 format, char* out, size_t out_size) {
    char fmt[2048], piece[1024], spec[48], str[1024];
    size_t len = 0;
    if (!out_size)
        return 0;
    out[0] = 0;
    if (!gcrt_guest_string(a->g, format, fmt, sizeof(fmt)))
        return 0;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            const char* q = strchr(p, '%');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            append(out, out_size, &len, p, n);
            p += n;
            continue;
        }
        const char* begin = p++;
        if (*p == '%') {
            append(out, out_size, &len, "%", 1);
            p++;
            continue;
        }
        /* flags, width, precision ("*": integer argument) */
        size_t s = 0;
        spec[s++] = '%';
        while (*p && strchr("-+ #0", *p) && s < 8)
            spec[s++] = *p++;
        if (*p == '*') {
            s += (size_t)snprintf(spec + s, sizeof(spec) - s, "%d", (int)next_u32(a));
            p++;
        } else {
            while (*p >= '0' && *p <= '9' && s < 20)
                spec[s++] = *p++;
        }
        if (*p == '.') {
            spec[s++] = *p++;
            if (*p == '*') {
                s += (size_t)snprintf(spec + s, sizeof(spec) - s, "%d", (int)next_u32(a));
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && s < 32)
                    spec[s++] = *p++;
            }
        }
        /* length: hh h l ll L */
        int longs = 0;
        bool shorts = false;
        while (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'z' || *p == 't' || *p == 'j') {
            if (*p == 'l' || *p == 'L' || *p == 'j')
                longs++;
            if (*p == 'h')
                shorts = true;
            p++;
        }
        char conv = *p ? *p++ : 0;
        piece[0] = 0;
        switch (conv) {
        case 'd':
        case 'i':
            if (longs >= 2 && conv) {
                spec[s++] = 'l';
                spec[s++] = 'l';
                spec[s++] = 'd';
                spec[s] = 0;
                snprintf(piece, sizeof(piece), spec, (long long)(s64)next_u64(a));
            } else {
                spec[s++] = 'd';
                spec[s] = 0;
                s32 v = (s32)next_u32(a);
                snprintf(piece, sizeof(piece), spec, shorts ? (int)(s16)v : (int)v);
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            if (longs >= 2) {
                spec[s++] = 'l';
                spec[s++] = 'l';
                spec[s++] = conv;
                spec[s] = 0;
                snprintf(piece, sizeof(piece), spec, (unsigned long long)next_u64(a));
            } else {
                spec[s++] = conv;
                spec[s] = 0;
                snprintf(piece, sizeof(piece), spec, (unsigned)next_u32(a));
            }
            break;
        case 'c':
            spec[s++] = 'c';
            spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, (int)(u8)next_u32(a));
            break;
        case 's': {
            u32 ptr = next_u32(a);
            if (!gcrt_guest_string(a->g, ptr, str, sizeof(str)))
                snprintf(str, sizeof(str), "(invalid string %08X)", ptr);
            spec[s++] = 's';
            spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, str);
            break;
        }
        case 'p':
            snprintf(piece, sizeof(piece), "0x%08x", next_u32(a));
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            spec[s++] = conv;
            spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, next_double(a));
            break;
        case 'n':
            next_u32(a); /* pointer ignored */
            break;
        default:
            /* unknown conversion: copied as is */
            append(out, out_size, &len, begin, (size_t)(p - begin));
            continue;
        }
        append(out, out_size, &len, piece, strlen(piece));
    }
    return len;
}
