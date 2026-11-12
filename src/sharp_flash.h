/* sharp_flash.h -- a Sharp LH28F-series (Intel 28F016SA-compatible) flash chip in byte mode, as the Braille Lite
 * Millennium's firmware drives its file flash: three of them, 4 MB each (the manual's 12 MB).
 *
 * The command set the firmware was seen to use: FFh read array, 70h read status, 71h read extended status (the
 * block's BSR at byte 2 of the block's address, the global GSR at byte 4), 50h clear status, 40h/10h byte write,
 * 20h + D0h block erase, 30h + D0h erase the whole chip, 90h read identifier.  Operations complete at once (the
 * firmware polls for ready either way).  Anything else is counted in `unknown` and ignored.
 */
#ifndef SHARP_FLASH_H
#define SHARP_FLASH_H

#include <stdint.h>

#define SF_SIZE 0x400000UL           /* 4 MB */
#define SF_BLOCK 0x10000UL           /* 64 KB blocks */

typedef struct {
    uint8_t *data;
    int mode;                        /* what a read returns: SF_ARRAY, SF_STATUS, SF_ESR, SF_ID */
    int pending;                     /* the first half of a two-cycle command (40h, 20h, 30h ...), or 0 */
    uint8_t status;                  /* the compatible status register (SR.7 ready, SR.5 erase, SR.4 write error) */
    unsigned long unknown;           /* commands not modelled */
    uint8_t last_unknown;
    int vpp;                         /* the programming voltage on (the board's switch); writes fail without it */
} sharp_flash;

enum { SF_ARRAY, SF_STATUS, SF_ESR, SF_ID };

int sf_init(sharp_flash *f);                       /* erased (all FFh); -1 if out of memory */
void sf_free(sharp_flash *f);
uint8_t sf_read(sharp_flash *f, uint32_t off);
void sf_write(sharp_flash *f, uint32_t off, uint8_t v);

#endif
