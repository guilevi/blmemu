/* blm_board.h -- the Braille Lite Millennium (M40) board around a Z180, as a library: one instance per unit.
 *
 * Found by running Freedom Scientific's own firmware (BLM40ENU.BNS, revision 4.00, February 2003) and reading what
 * it does; no schematic.  What the firmware touches, and what is modelled:
 *
 *   00000h-7FFFFh  the program flash, 512 KB: the .BNS file from its image (F3 C3 .. "COPYRIGHT") on is a linear
 *                  copy of it -- the program at 00000h, its messages at 50000h.  E0h bit 1 picks bank A or B (the
 *                  image is in both).  Never written in ordinary use (the firmware update is not modelled).
 *   80000h-83FFFh  the first 16 KB of the 2 MB RAM (the manual's 2 MB), always.
 *   84000h-DFFFFh  a window paged by F2h:F1h:F0h (address bits 8-31, 4 KB steps; power-on 01800000h, which is the
 *                  RAM as it is: the RAM decodes 21 address lines).
 *   E0000h-FFFFFh  a 128 KB window paged by F6h:F5h (address bits 16-31).  The address space behind both:
 *                  01xxxxxxh the RAM; 0040_0000h-00FF_FFFFh the file flash, three 4 MB Sharp LH28F-style chips
 *                  (F5h 40h, 80h, C0h: the manual's 12 MB; sharp_flash.h).  80h bit 2 is their programming
 *                  voltage; dropping it puts them back to reading.
 *   E0h            a latch, read back, with inputs: bit 1 the program bank, bit 2 the clock's chip select (out);
 *                  bit 6 the power switch, bit 3 the Braille Display Mode switch (in, both active high = on / off).
 *   8Ch            the keyboard controller, a microcontroller that shifts bytes to the Z180: bit 5 low = a byte waits
 *                  (and /INT2), bit 1 the Z180's clock (a bit per rising edge, most significant first), bit 0 the
 *                  data, bit 7 low resets it.  Bytes come in pairs (kind, value): 05h the dot keys down (dot 1 =
 *                  bit 0 .. dot 8 = bit 7), 04h the space bar (bit 0); a chord is what was down before all came up.
 *                  02h a cursor routing key, 08h the advance bars and Whiz Wheels (not mapped yet: blm_key_raw).
 *   88h            the braille display: 40 cells of shift registers, bit 0 data, bit 1 clock, bit 2 latch; the
 *                  rightmost cell first, the Braille Lite 2000's wiring of dots.
 *   CSI/O          an SPI real-time clock (DS1305-like: address byte, bit 7 = write; BCD seconds .. year at 00h-06h),
 *                  selected by E0h bit 2, most significant bit first (the firmware bit-reverses).
 *   90h            the DoubleTalk (RC8650) on its parallel bus: writes its input, reads its status register (SR.6
 *                  talking, SR.4 ready, SR.2 almost empty; blm_dt.h).  91h-94h are written as an SSI-263 by some
 *                  routines inherited from the Braille Lite 2000; the M40's own paths do not use them.
 *   A0h-A2h, AAh   the parallel printer port (data, status, control); 80h bit 5 powers it.  Not connected.
 *   ASCI0          the serial port (PC, Braille Display Mode).  Not connected yet.
 *
 * Not thread-safe per instance.
 */
#ifndef BLM_BOARD_H
#define BLM_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blm_unit blm_unit;

#define BLM_MAX_CELLS 64

/* What the board needs from its host: the DoubleTalk.  write: a byte the firmware wrote to it; status: its status
   register now (bit 6 talking, bit 4 ready, bit 2 almost empty).  NULL members: bytes dropped, always ready. */
typedef struct {
    void *ctx;
    void (*dt_write)(void *ctx, uint8_t byte);
    uint8_t (*dt_status)(void *ctx);
} blm_host;

/* image: the 256 KB program (the .BNS update's image, blm_find_image).  NULL on failure. */
blm_unit *blm_create(const uint8_t *image, long len, const blm_host *host);
void blm_destroy(blm_unit *u);

/* Finds the program in a .BNS update file: its offset, or -1.  The program starts F3 C3 xx xx FF "COPYRIGHT". */
long blm_find_image(const uint8_t *data, long n);

void blm_run(blm_unit *u, uint64_t cycles);        /* run that many CPU cycles (6.144 MHz) */
uint64_t blm_cycles(const blm_unit *u);
double blm_clock_hz(const blm_unit *u);

/* Keys.  blm_keys: the dot keys (bit 0 = dot 1 .. bit 7 = dot 8) and the space bar down now; the controller sends
   what changed.  blm_chord: pressed and released, as a person would. */
void blm_keys(blm_unit *u, int dots, int space);
void blm_chord(blm_unit *u, int dots, int space);
int blm_keys_pending(const blm_unit *u);
void blm_key_raw(blm_unit *u, int kind, int value);   /* one (kind, value) pair from the controller as it is */          /* bytes the controller has yet to send */

/* The braille display as last latched, left to right, dot 1 = bit 0 .. dot 8 = bit 7.  Returns the count (40). */
int blm_cells(const blm_unit *u, uint8_t *cells, int cap);
int blm_display_latches(const blm_unit *u);        /* how many times it has latched */

/* The power switch.  Off: the firmware notices on its next 10 ms tick, saves what it keeps, and stops
   (blm_powered_down then says so; save the state after that).  On: the CPU starts again from its reset vector with
   the RAM as it was, as the unit does. */
void blm_power(blm_unit *u, int on);
int blm_powered_down(const blm_unit *u);

/* The Braille Display Mode switch on the left side (the manual, "M40 Ports and Switches"): on (toward the front),
   the unit is a braille display for a PC's screen reader; off (toward the back, the default), a note-taker.  Read on
   E0h bit 3, low when on. */
void blm_display_mode_switch(blm_unit *u, int on);
int blm_display_mode(const blm_unit *u);

/* The battery-backed RAM (and the clock's registers): saved and restored whole. */
int blm_save(const blm_unit *u, const char *path);
int blm_load(blm_unit *u, const char *path);

/* A newly made unit's RAM holds nothing the firmware wrote.  The real units left the factory with a hard reset done
   (I-chord at power-on); blm_factory does that: the timer reload a cold start sets (or the 10 ms tick runs at full
   speed and the unit never gets past it), then I-chord held through the start. */
void blm_factory(blm_unit *u);

/* Tracing: a FILE * (stdio) for port and far-call logs; NULL off. */
void blm_trace(blm_unit *u, void *fp, int what);
enum { BLM_TR_IO = 1, BLM_TR_FAR = 2, BLM_TR_DT = 4, BLM_TR_KEYS = 8, BLM_TR_UNKNOWN = 16, BLM_TR_INTERNAL = 32, BLM_TR_MEM = 64, BLM_TR_FLASH = 128 };

void blm_fetch_stats(blm_unit *u, void *fp);        /* opcode fetches per 256 bytes since the last call */
void blm_mem_stats(const blm_unit *u, void *fp);   /* accesses per 4 KB page above the program */
uint32_t blm_pc(const blm_unit *u);                /* logical */
uint32_t blm_phys_pc(const blm_unit *u);           /* the last opcode fetch, physical */
uint8_t blm_peek(const blm_unit *u, uint32_t phys);
void blm_poke(blm_unit *u, uint32_t phys, uint8_t v);

#ifdef __cplusplus
}
#endif
#endif
