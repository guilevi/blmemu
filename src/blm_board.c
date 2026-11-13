/* blm_board.c -- the Braille Lite Millennium board (see blm_board.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu/cpu.h"
#include "blm_board.h"
#include "sharp_flash.h"
#include "u8file.h"

#define ROM_SIZE 0x80000UL                   /* one bank of the program flash: 00000h-7FFFFh */
#define RAM_SIZE 0x200000UL                  /* 2 MB (the manual) */
#define RAM_BASE 0x80000UL                   /* 80000h-83FFFh: the RAM's first 16 KB, always */
#define WIN2_BASE 0x84000UL                  /* 84000h-DFFFFh: paged by F2h:F1h:F0h (power-on: the RAM, as is) */
#define WIN_BASE 0xE0000UL                   /* E0000h-FFFFFh: a 128 KB window, paged by F6h:F5h */
#define N_FFLASH 3
#define CLOCK_HZ 6144000.0
#define KQ 256
#define KEY_GAP_CYCLES 6144          /* the keyboard controller's pace between bytes: 1 ms (model) */
#define CSIO_CYCLES 320              /* a CSI/O byte at the internal clock: 8 bits at PHI/40 (the firmware's SS) */
#define SAVE_MAGIC "BLMSTATE2"

typedef struct {                      /* the DS1305-like clock on the CSI/O */
    uint8_t reg[0x80];                /* 00h-06h computed from the time; the rest as written */
    long long base;                   /* seconds since 1970 (as calendar fields, no zone) at cycle c0 */
    uint64_t c0;
    int selected, first, addr, write;
} blm_rtc;

struct blm_unit {
    uint8_t *flash;                   /* the program flash, both banks */
    uint8_t *ram;                     /* 2 MB */
    sharp_flash ff[N_FFLASH];         /* the file flash: F5h 40h-7Fh, 80h-BFh, C0h-FFh with F6h = 00h */
    z180 *cpu;
    blm_host host;
    uint8_t port_e0, port_80;
    int power_off;                    /* the power switch, off (E0h bit 6 low) */
    int display_mode;                 /* the Braille Display Mode switch toward the front: E0h bit 3 low (the
                                         firmware says "braille display mode on" and serves a PC's screen reader on
                                         the serial port until the switch goes back) */
    /* keyboard controller */
    uint8_t kq[KQ];
    int kq_h, kq_t, k_bits, k_cur, k_busy, k_clk, k_data;
    uint64_t k_next;
    int k_dots, k_space;
    int k_hold;                       /* held in reset (8Ch bit 7 low) */              /* what the controller last reported */
    /* CSI/O transfer under way (internal clock) */
    uint8_t port_f[8];                /* F0h-F7h as written (the windows' page registers) */
    uint64_t dt_busy_until;
    /* the braille display: a chain of shift registers on 88h (bit 0 data, bit 1 clock, bit 2 latch) */
    uint8_t disp_bits[1024];          /* the bits shifted in since the last latch, first one first */
    int disp_n, disp_clk;
    uint8_t cells[BLM_MAX_CELLS];
    int n_cells, disp_latches;
    uint64_t csio_done;
    int csio_armed;
    blm_rtc rtc;
    uint32_t last_fetch;
    FILE *tr;
    int tr_what;
    unsigned long unknown_n[256][2];
    uint8_t far_b[3];
    unsigned long page_r[256], page_w[256];
    unsigned long fetch_hist[4096];          /* opcode fetches per 256 bytes (blm_fetch_stats) */   /* accesses per 4 KB page (blm_mem_stats) */
};

/* ---- the clock ------------------------------------------------------------------------------------------------- */
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }
static int unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

static long long days_from_civil(int y, int m, int d)   /* Howard Hinnant's */
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, int *y, int *m, int *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static long long rtc_now(const blm_unit *u)
{
    return u->rtc.base + (long long)((double)(z180_cycles(u->cpu) - u->rtc.c0) / CLOCK_HZ);
}

static void rtc_fields(const blm_unit *u, uint8_t *out)   /* 00h-06h */
{
    long long t = rtc_now(u), days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int s = (int)(t - days * 86400), y, m, d;
    civil_from_days(days, &y, &m, &d);
    out[0] = bcd(s % 60);
    out[1] = bcd(s / 60 % 60);
    out[2] = bcd(s / 3600);                                  /* 24-hour mode */
    out[3] = (uint8_t)((days + 4) % 7 + 1 + ((days + 4) % 7 < 0 ? 7 : 0)); /* 1 = Sunday (1970-01-01 a Thursday) */
    out[4] = bcd(d);
    out[5] = bcd(m);
    out[6] = bcd(y % 100);
}

static void rtc_set_field(blm_unit *u, int reg, uint8_t v)
{
    uint8_t f[7];
    rtc_fields(u, f);
    f[reg] = v;
    int y = 2000 + unbcd(f[6]), m = unbcd(f[5] & 0x1F), d = unbcd(f[4] & 0x3F);
    int hh = unbcd(f[2] & 0x3F), mm = unbcd(f[1] & 0x7F), ss = unbcd(f[0] & 0x7F);
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    u->rtc.base = days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
    u->rtc.c0 = z180_cycles(u->cpu);
}

static uint8_t rtc_read(blm_unit *u, int reg)
{
    if (reg < 7) {
        uint8_t f[7];
        rtc_fields(u, f);
        return f[reg];
    }
    return u->rtc.reg[reg & 0x7F];
}

/* one byte over SPI while the clock is selected: `in` from the Z180 (MSB first, already un-reversed); returns what
   the clock drives back */
static uint8_t rtc_byte(blm_unit *u, uint8_t in, int te)
{
    blm_rtc *r = &u->rtc;
    if (r->first) {
        if (!te) return 0xFF;
        r->first = 0;
        r->write = (in & 0x80) != 0;
        r->addr = in & 0x7F;
        return 0xFF;
    }
    if (r->write && te) {
        if (r->addr < 7) rtc_set_field(u, r->addr, in);
        else r->reg[r->addr] = in;
        r->addr = (r->addr + 1) & 0x7F;
        return 0xFF;
    }
    if (!r->write) {
        uint8_t v = rtc_read(u, r->addr);
        r->addr = (r->addr + 1) & 0x7F;
        return v;
    }
    return 0xFF;
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* ---- memory above the program: the RAM, and the window onto the RAM and the file flash ---------------------------- */
/* phys (80000h-FFFFFh) -> what answers there: 1 with *ram set for the RAM, or *ram NULL and *chip and *off for a
   flash chip; 2 with *ram on the program flash (read only); 0 nothing */
static int ext_decode(blm_unit *u, uint32_t a, uint8_t **ram, int *chip, uint32_t *off)
{
    uint32_t ext;
    if (a < WIN2_BASE) { *ram = u->ram + (a - RAM_BASE); return 1; }
    if (a < WIN_BASE)                    /* F2h:F1h:F0h, address bits 8-31 (4 KB steps: F0h's low nibble unused) */
        ext = ((((uint32_t)u->port_f[2] << 16 | (uint32_t)u->port_f[1] << 8 | u->port_f[0]) << 8) & ~0xFFFUL) +
              (a - RAM_BASE);
    else
        /* F6h:F5h are address bits 16-31 and the CPU gives bits 0-15, so both halves of the window (E0000h and
           F0000h) show the same 64 KB.  Found from the Help file: the firmware sets F6h:F5h = 0005h and reads
           F36F8h on, 1,369 bytes, and its "mini help file", 1,369 bytes, is at 536F8h of the program flash. */
        ext = (((uint32_t)u->port_f[6] << 8 | u->port_f[5]) << 16) + (a & 0xFFFF);
    if (ext < ROM_SIZE) {                /* the program flash, as the CPU sees it below 80000h: its messages, read
                                            through the windows (the Help file's text) */
        *ram = u->flash + ((u->port_e0 & 0x02) ? ROM_SIZE : 0) + ext;
        return 2;
    }
    if ((ext >> 24) == 1) {              /* the RAM: 21 address lines, so it repeats through 01000000h-01FFFFFFh */
        *ram = u->ram + (ext & (RAM_SIZE - 1));
        return 1;
    }
    if ((ext >> 24) == 0 && (ext >> 22) >= 1) {
        *ram = NULL;
        *chip = (int)(ext >> 22) - 1;
        *off = ext & (SF_SIZE - 1);
        return 1;
    }
    return 0;
}

static uint8_t ext_read(blm_unit *u, uint32_t a)
{
    uint8_t *r;
    int chip = 0;
    uint32_t off = 0;
    if (!ext_decode(u, a, &r, &chip, &off)) {
        if (u->tr && (u->tr_what & BLM_TR_UNKNOWN))
            fprintf(u->tr, "MR? %05X F6:F5=%02X%02X pc=%05X\n", (unsigned)a, u->port_f[6], u->port_f[5],
                    (unsigned)u->last_fetch);
        return 0xFF;
    }
    if (r) return *r;
    return sf_read(&u->ff[chip], off);
}

static void ext_write(blm_unit *u, uint32_t a, uint8_t v)
{
    uint8_t *r;
    int chip = 0, what;
    uint32_t off = 0;
    what = ext_decode(u, a, &r, &chip, &off);
    if (what != 1) {                     /* nothing there, or the program flash (never written in ordinary use) */
        if (u->tr && (u->tr_what & BLM_TR_UNKNOWN))
            fprintf(u->tr, "MW? %05X <- %02X F6:F5=%02X%02X pc=%05X\n", (unsigned)a, v, u->port_f[6], u->port_f[5],
                    (unsigned)u->last_fetch);
        return;
    }
    if (r) { *r = v; return; }
    u->ff[chip].vpp = (u->port_80 & 0x04) != 0;
    if (u->tr && (u->tr_what & BLM_TR_FLASH))
        fprintf(u->tr, "FF%d %06X <- %02X pc=%05X\n", chip, (unsigned)off, v, (unsigned)u->last_fetch);
    sf_write(&u->ff[chip], off, v);
}

/* ---- the bus ----------------------------------------------------------------------------------------------------- */
static uint8_t mem_read(void *ctx, uint32_t a)
{
    blm_unit *u = (blm_unit *)ctx;
    uint8_t v;
    a &= 0xFFFFF;
    u->page_r[a >> 12]++;
    if (a >= 0x84000 && u->tr && (u->tr_what & BLM_TR_MEM) && a != u->last_fetch)
        fprintf(u->tr, "MR %05X F0-2=%02X%02X%02X F5-6=%02X%02X pc=%05X\n", (unsigned)a, u->port_f[2], u->port_f[1],
                u->port_f[0], u->port_f[6], u->port_f[5], (unsigned)u->last_fetch);
    v = a < ROM_SIZE ? u->flash[((u->port_e0 & 0x02) ? ROM_SIZE : 0) + a] : ext_read(u, a);
    if (u->tr && (u->tr_what & BLM_TR_FAR)) {        /* the far-call stub's operands: CALL 63F0h; dw target; db bbr */
        uint32_t pc = z180_pc(u->cpu);
        if (pc == 0x63F4) u->far_b[0] = v;
        else if (pc == 0x63F6) u->far_b[1] = v;
        else if (pc == 0x63FB) u->far_b[2] = v;
    }
    return v;
}

static uint8_t mem_fetch(void *ctx, uint32_t a)
{
    blm_unit *u = (blm_unit *)ctx;
    u->last_fetch = a & 0xFFFFF;
    u->fetch_hist[u->last_fetch >> 8]++;
    return mem_read(ctx, a);
}

static void mem_write(void *ctx, uint32_t a, uint8_t v)
{
    blm_unit *u = (blm_unit *)ctx;
    a &= 0xFFFFF;
    u->page_w[a >> 12]++;
    if (a >= 0x84000 && u->tr && (u->tr_what & BLM_TR_MEM))
        fprintf(u->tr, "MW %05X <- %02X F0-2=%02X%02X%02X F5-6=%02X%02X pc=%05X\n", (unsigned)a, v, u->port_f[2],
                u->port_f[1], u->port_f[0], u->port_f[6], u->port_f[5], (unsigned)u->last_fetch);
    if (a < ROM_SIZE) {
        if (u->tr && (u->tr_what & BLM_TR_UNKNOWN))
            fprintf(u->tr, "FLASHW %05X <- %02X pc=%05X\n", (unsigned)a, v, (unsigned)u->last_fetch);
        return;
    }
    ext_write(u, a, v);
}

static int internal_port(uint16_t p) { return (p & 0xFF00) == 0 && (p & 0xC0) == 0; }

static void unknown(blm_unit *u, int wr, uint16_t p, uint8_t v)
{
    if (u->tr && (u->tr_what & BLM_TR_UNKNOWN) && u->unknown_n[p & 0xFF][wr]++ < 8)
        fprintf(u->tr, "%s %02X %s %02X pc=%04X/%05X cyc=%llu\n", wr ? "OUT" : "IN ", p & 0xFF, wr ? "<-" : "->", v,
                (unsigned)z180_pc(u->cpu), (unsigned)u->last_fetch, (unsigned long long)z180_cycles(u->cpu));
}

static uint8_t io_read(void *ctx, uint16_t port)
{
    blm_unit *u = (blm_unit *)ctx;
    int p = port & 0xFF;
    uint8_t v = 0xFF;
    if (internal_port(port))
        return 0xFF;                   /* the core answers its own registers; this is the bus's echo */
    switch (p) {
    case 0x8C:
        v = (uint8_t)(0xDE | (u->k_busy ? 0 : 0x20) | u->k_data);
        break;
    case 0x90:
        v = u->host.dt_status ? u->host.dt_status(u->host.ctx) : 0x14;
        if (z180_cycles(u->cpu) < u->dt_busy_until)
            v &= (uint8_t)~0x10;       /* RDY drops briefly after each write: the chip's receipt (datasheet, 8 us min.) */
        break;
    case 0xE0:
        /* bit 6: the power switch, on (low: the firmware powers the unit down); bit 3 an input the firmware reads
           (F36h), low here */
        v = (uint8_t)((u->port_e0 & ~0x48) | (u->power_off ? 0 : 0x40) | (u->display_mode ? 0 : 0x08));
        break;
    case 0xF0: case 0xF1: case 0xF2: case 0xF5: case 0xF6:
        v = u->port_f[p & 7];
        break;
    case 0x84:
        /* bit 1: the battery is low (the half-second tick at 0807h: IN0 A,(84h), BIT 1,A, and when set a flag at
           D40Fh that has the unit say "battery low" after 88 s and every 125 s from then on).  A unit on a good
           battery reads it clear.  The other bits are not read there; left high. */
        v = 0xFD;
        break;
    case 0xD0:                         /* read in every delay loop and the key handler: its value is thrown away */
        break;
    default:
        unknown(u, 0, port, v);
    }
    if (u->tr && (u->tr_what & BLM_TR_IO))
        fprintf(u->tr, "IN  %02X -> %02X pc=%05X\n", p, v, (unsigned)u->last_fetch);
    return v;
}

static void key_reset(blm_unit *u)
{
    if (u->k_busy)
        u->kq_t--;                     /* the byte being shifted is sent again after the reset */
    u->k_busy = 0;
    u->k_bits = 0;
    z180_set_irq(u->cpu, Z180_INT2, 0);
}

static void io_write(void *ctx, uint16_t port, uint8_t v)
{
    blm_unit *u = (blm_unit *)ctx;
    int p = port & 0xFF;
    if (internal_port(port)) {
        if (u->tr && (u->tr_what & BLM_TR_INTERNAL) && !(p >= 0x0C && p <= 0x13) && !(p >= 0x38 && p <= 0x3A))
            fprintf(u->tr, "OUT0 %02X <- %02X pc=%05X cyc=%llu\n", p, v, (unsigned)u->last_fetch,
                    (unsigned long long)z180_cycles(u->cpu));
        return;
    }
    if (u->tr && (u->tr_what & BLM_TR_IO))
        fprintf(u->tr, "OUT %02X <- %02X pc=%05X\n", p, v, (unsigned)u->last_fetch);
    switch (p) {
    case 0x8C: {
        int clk = (v >> 1) & 1;
        if (!(v & 0x80) && !u->k_hold)
            key_reset(u);
        u->k_hold = !(v & 0x80);
        if (!u->k_hold && clk && !u->k_clk && u->k_busy) {
            u->k_data = (u->k_cur >> (7 - u->k_bits)) & 1;
            if (++u->k_bits == 8) {
                u->k_busy = 0;
                z180_set_irq(u->cpu, Z180_INT2, 0);
                u->k_next = z180_cycles(u->cpu) + KEY_GAP_CYCLES;
            }
        }
        u->k_clk = clk;
        break;
    }
    case 0x90:
        if (u->tr && (u->tr_what & BLM_TR_DT))
            fprintf(u->tr, "DT %02X %c\n", v, v >= 32 && v < 127 ? v : '.');
        if (u->host.dt_write)
            u->host.dt_write(u->host.ctx, v);
        u->dt_busy_until = z180_cycles(u->cpu) + 60;
        break;
    case 0x88: {
        int clk = (v >> 1) & 1;
        if (clk && !u->disp_clk && u->disp_n < (int)sizeof u->disp_bits)
            u->disp_bits[u->disp_n++] = v & 1;
        u->disp_clk = clk;
        if (v & 0x04) {
            /* latch.  The rightmost cell goes first, a byte per cell, least significant bit first; the wire's bits
               are dots 7, 3, 2, 1, 8, 6, 5, 4 (the Braille Lite 2000's piezo drivers, wired the same way: "INITIALIZE"
               in grade 2 reads in-i-t-i-a-l-i-z-e with it) */
            static const uint8_t dot_of_bit[8] = {0x40, 0x04, 0x02, 0x01, 0x80, 0x20, 0x10, 0x08};
            int n = u->disp_n / 8, i, b;
            if (n > BLM_MAX_CELLS) n = BLM_MAX_CELLS;
            for (i = 0; i < n; i++) {
                const uint8_t *w = u->disp_bits + (n - 1 - i) * 8;
                uint8_t c = 0;
                for (b = 0; b < 8; b++)
                    if (w[b]) c |= dot_of_bit[b];
                u->cells[i] = c;
            }
            if (n) { u->n_cells = n; u->disp_latches++; }
            u->disp_n = 0;
        }
        break;
    }
    case 0x80:
        if ((u->port_80 & 0x04) && !(v & 0x04))      /* the file flash's RP#/VPP dropped: the chips back to reading */
            for (int i = 0; i < N_FFLASH; i++) { u->ff[i].mode = SF_ARRAY; u->ff[i].pending = 0; }
        u->port_80 = v;               /* bit 2: the file flash's programming voltage; bit 5 the printer port's power */
        unknown(u, 1, port, v);
        break;
    case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        u->port_f[p & 7] = v;
        if (u->tr && (u->tr_what & BLM_TR_UNKNOWN))
            fprintf(u->tr, "PAGE F%d <- %02X pc=%05X\n", p & 7, v, (unsigned)u->last_fetch);
        break;
    case 0xE0: {
        int was = u->port_e0 & 0x04;
        u->port_e0 = v;
        if ((v & 0x04) && !was) {      /* the clock selected: a new transfer, its address first */
            u->rtc.selected = 1;
            u->rtc.first = 1;
        } else if (!(v & 0x04))
            u->rtc.selected = 0;
        break;
    }
    default:
        unknown(u, 1, port, v);
    }
}

static void boundary(void *ctx, uint32_t pc)
{
    blm_unit *u = (blm_unit *)ctx;
    uint64_t now = z180_cycles(u->cpu);
    (void)pc;
    /* the keyboard controller's next byte */
    if (!u->k_busy && !u->k_hold && u->kq_t != u->kq_h && now >= u->k_next) {
        u->k_cur = u->kq[u->kq_t++ % KQ];
        u->k_busy = 1;
        u->k_bits = 0;
        u->k_data = 0;
        z180_set_irq(u->cpu, Z180_INT2, 1);
        if (u->tr && (u->tr_what & BLM_TR_KEYS))
            fprintf(u->tr, "KEY %02X\n", u->k_cur);
    }
    /* the CSI/O on its own clock: a byte takes CSIO_CYCLES from the moment TE or RE is set */
    {
        uint8_t cntr = z180_csio_cntr(u->cpu);
        int armed = cntr & 0x30;
        if (armed && (cntr & 7) != 7) {
            if (!u->csio_armed) {
                u->csio_armed = 1;
                u->csio_done = now + CSIO_CYCLES;
            } else if (now >= u->csio_done) {
                uint8_t sent = 0xFF, in = 0xFF;
                /* what goes out is TRDR (if TE); what the clock returns depends on it, so ask the clock first with
                   the outgoing byte (TRDR is not readable without side effects: the core hands it over here) */
                uint8_t dummy;
                int te = armed & 0x10;
                if (te) {
                    z180_csio_clock(u->cpu, 0xFF, &sent);
                    if (u->rtc.selected) rtc_byte(u, rev8(sent), 1);
                } else {
                    if (u->rtc.selected) in = rev8(rtc_byte(u, 0xFF, 0));
                    z180_csio_clock(u->cpu, in, &dummy);
                }
                u->csio_armed = 0;
            }
        } else
            u->csio_armed = 0;
    }
    if (u->tr && (u->tr_what & BLM_TR_FAR) && pc == 0x63FD) {
        unsigned t = (unsigned)(u->far_b[0] | u->far_b[1] << 8);
        fprintf(u->tr, "FAR %llu -> %04X bank %02X (phys %05X)\n", (unsigned long long)now, t, u->far_b[2],
                t + u->far_b[2] * 0x1000u);
    }
}

static int serial_rx(void *ctx, int ch) { (void)ctx; (void)ch; return -1; }
static void serial_tx(void *ctx, int ch, uint8_t b)
{
    blm_unit *u = (blm_unit *)ctx;
    if (u->tr && (u->tr_what & BLM_TR_UNKNOWN))
        fprintf(u->tr, "TX%d %02X\n", ch, b);
}

/* ---- the unit ---------------------------------------------------------------------------------------------------- */
long blm_find_image(const uint8_t *data, long n)
{
    static const char notice[] = "COPYRIGHT";
    long i;
    for (i = 0; i + 5 + (long)sizeof notice - 1 <= n; i++)
        if (data[i] == 0xF3 && data[i + 1] == 0xC3 && data[i + 4] == 0xFF &&
            memcmp(data + i + 5, notice, sizeof notice - 1) == 0)
            return i;
    return -1;
}

blm_unit *blm_create(const uint8_t *image, long len, const blm_host *host)
{
    blm_unit *u = (blm_unit *)calloc(1, sizeof *u);
    cpu_bus bus;
    time_t t;
    struct tm *lt;
    if (!u) return NULL;
    u->flash = (uint8_t *)malloc(2 * ROM_SIZE);
    u->ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!u->flash || !u->ram) { blm_destroy(u); return NULL; }
    for (int i = 0; i < N_FFLASH; i++)
        if (sf_init(&u->ff[i])) { blm_destroy(u); return NULL; }
    memset(u->flash, 0xFF, 2 * ROM_SIZE);
    if (len > (long)ROM_SIZE) len = (long)ROM_SIZE;
    memcpy(u->flash, image, (size_t)len);
    memcpy(u->flash + ROM_SIZE, image, (size_t)len);
    if (host) u->host = *host;
    memset(&bus, 0, sizeof bus);
    bus.ctx = u;
    bus.read = mem_read;
    bus.fetch = mem_fetch;
    bus.write = mem_write;
    bus.in = io_read;
    bus.out = io_write;
    bus.boundary = boundary;
    bus.serial_rx = serial_rx;
    bus.serial_tx = serial_tx;
    u->cpu = z180_create(&bus, CLOCK_HZ);
    if (!u->cpu) { blm_destroy(u); return NULL; }
    z180_reset(u->cpu);
    t = time(NULL);
    lt = localtime(&t);
    u->rtc.base = days_from_civil(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday) * 86400 + lt->tm_hour * 3600 +
                  lt->tm_min * 60 + lt->tm_sec;
    u->port_f[0] = 0x00;              /* the second window's power-on page: the RAM as it is (the firmware's own value) */
    u->port_f[1] = 0x80;
    u->port_f[2] = 0x01;
    u->port_f[6] = 0x01;
    u->rtc.reg[0x0F] = 0x40;          /* control: oscillator on, write-protected (DS1305 power-on) */
    return u;
}

void blm_destroy(blm_unit *u)
{
    if (!u) return;
    if (u->cpu) z180_destroy(u->cpu);
    free(u->flash);
    free(u->ram);
    for (int i = 0; i < N_FFLASH; i++) sf_free(&u->ff[i]);
    free(u);
}

void blm_run(blm_unit *u, uint64_t cycles)
{
    uint64_t end = z180_cycles(u->cpu) + cycles;
    while (z180_cycles(u->cpu) < end)
        z180_step(u->cpu);
}

uint64_t blm_cycles(const blm_unit *u) { return z180_cycles(u->cpu); }
double blm_clock_hz(const blm_unit *u) { (void)u; return CLOCK_HZ; }
uint32_t blm_pc(const blm_unit *u) { return z180_pc(u->cpu); }
uint32_t blm_phys_pc(const blm_unit *u) { return u->last_fetch; }
uint8_t blm_peek(const blm_unit *u, uint32_t a)
{
    a &= 0xFFFFF;
    if (a < ROM_SIZE) return u->flash[a];
    return a < WIN2_BASE ? u->ram[a - RAM_BASE] : 0xFF;
}
void blm_poke(blm_unit *u, uint32_t a, uint8_t v) { a &= 0xFFFFF; if (a >= RAM_BASE && a < WIN2_BASE) u->ram[a - RAM_BASE] = v; }

static void kpush(blm_unit *u, int b)
{
    if (u->kq_h - u->kq_t < KQ) u->kq[u->kq_h++ % KQ] = (uint8_t)b;
}

void blm_keys(blm_unit *u, int dots, int space)
{
    dots &= 0xFF;
    space = space ? 1 : 0;
    if (dots != u->k_dots) { kpush(u, 0x05); kpush(u, dots); u->k_dots = dots; }
    if (space != u->k_space) { kpush(u, 0x04); kpush(u, space); u->k_space = space; }
}

void blm_chord(blm_unit *u, int dots, int space)
{
    blm_keys(u, dots, space);
    blm_keys(u, 0, 0);
}

int blm_keys_pending(const blm_unit *u) { return u->kq_h - u->kq_t + u->k_busy; }

void blm_factory(blm_unit *u)
{
    blm_poke(u, 0x81644, 0x7C);       /* D644h in common area 1 (CBR 74h): the PRT0 reload, 927Ch */
    blm_poke(u, 0x81645, 0x92);
    blm_keys(u, 0x0A, 1);             /* I-chord (dots 2-4 and space) held at power-on: the hard reset */
    u->k_next = 0;
}

void blm_trace(blm_unit *u, void *fp, int what)
{
    u->tr = (FILE *)fp;
    u->tr_what = what;
}

int blm_save(const blm_unit *u, const char *path)
{
    FILE *f = u8_fopen(path, "wb");
    long long now;
    if (!f) return -1;
    now = rtc_now(u);
    fwrite(SAVE_MAGIC, 1, sizeof SAVE_MAGIC, f);
    fwrite(u->ram, 1, RAM_SIZE, f);
    for (int i = 0; i < N_FFLASH; i++) fwrite(u->ff[i].data, 1, SF_SIZE, f);
    fwrite(u->rtc.reg, 1, sizeof u->rtc.reg, f);
    fwrite(&now, sizeof now, 1, f);
    return fclose(f);
}

int blm_load(blm_unit *u, const char *path)
{
    FILE *f = u8_fopen(path, "rb");
    char magic[sizeof SAVE_MAGIC];
    long long saved;
    if (!f) return -1;
    if (fread(magic, 1, sizeof magic, f) != sizeof magic || memcmp(magic, SAVE_MAGIC, sizeof magic) ||
        fread(u->ram, 1, RAM_SIZE, f) != RAM_SIZE || fread(u->ff[0].data, 1, SF_SIZE, f) != SF_SIZE ||
        fread(u->ff[1].data, 1, SF_SIZE, f) != SF_SIZE || fread(u->ff[2].data, 1, SF_SIZE, f) != SF_SIZE ||
        fread(u->rtc.reg, 1, sizeof u->rtc.reg, f) != sizeof u->rtc.reg || fread(&saved, sizeof saved, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    (void)saved;                       /* the clock keeps the host's time: it ran on its battery meanwhile */
    return 0;
}

void blm_mem_stats(const blm_unit *u, void *fp)
{
    int i;
    for (i = 0x40; i < 256; i++)
        if (u->page_r[i] || u->page_w[i])
            fprintf((FILE *)fp, "page %05X r=%lu w=%lu\n", i << 12, u->page_r[i], u->page_w[i]);
}

void blm_fetch_stats(blm_unit *u, void *fp)
{
    int i;
    for (i = 0; i < 4096; i++)
        if (u->fetch_hist[i]) fprintf((FILE *)fp, "fetch %05X %lu\n", i << 8, u->fetch_hist[i]);
    memset(u->fetch_hist, 0, sizeof u->fetch_hist);
}

int blm_cells(const blm_unit *u, uint8_t *cells, int cap)
{
    int n = u->n_cells < cap ? u->n_cells : cap;
    memcpy(cells, u->cells, (size_t)n);
    return n;
}

int blm_display_latches(const blm_unit *u) { return u->disp_latches; }

void blm_power(blm_unit *u, int on)
{
    if (on && u->power_off) {          /* switched on: the CPU starts from its reset vector, the RAM as it was */
        u->power_off = 0;
        z180_reset(u->cpu);
        u->port_e0 = 0;
        u->kq_t = u->kq_h;
        u->k_busy = u->k_hold = 0;
        u->k_dots = u->k_space = 0;
        u->csio_armed = 0;
        u->disp_n = 0;
    } else if (!on)
        u->power_off = 1;              /* the firmware sees it on its next tick and powers itself down */
}

int blm_powered_down(const blm_unit *u)
{
    /* after the switch goes off the firmware writes 80h = 08h, then 00h, and loops there (0844h) */
    return u->power_off && u->port_80 == 0x00 && u->last_fetch >= 0x00843 && u->last_fetch <= 0x00848;
}

void blm_key_raw(blm_unit *u, int kind, int value)
{
    kpush(u, kind);
    kpush(u, value);
}

void blm_display_mode_switch(blm_unit *u, int on) { u->display_mode = on != 0; }
int blm_display_mode(const blm_unit *u) { return u->display_mode; }
