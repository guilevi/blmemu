/* sharp_flash.c -- see sharp_flash.h. */
#include <stdlib.h>
#include <string.h>
#include "sharp_flash.h"

int sf_init(sharp_flash *f)
{
    memset(f, 0, sizeof *f);
    f->data = (uint8_t *)malloc(SF_SIZE);
    if (!f->data) return -1;
    memset(f->data, 0xFF, SF_SIZE);
    f->status = 0x80;
    f->vpp = 1;
    return 0;
}

void sf_free(sharp_flash *f)
{
    free(f->data);
    f->data = NULL;
}

uint8_t sf_read(sharp_flash *f, uint32_t off)
{
    off &= SF_SIZE - 1;
    switch (f->mode) {
    case SF_STATUS:
        return f->status;
    case SF_ESR:
        if ((off & 7) == 2) return (uint8_t)(0x80 | (f->status & 0x30 ? 0x20 : 0));   /* BSR: ready, op status */
        if ((off & 7) == 4) return (uint8_t)(0x80 | (f->status & 0x30 ? 0x20 : 0));   /* GSR: ready, dev status */
        return 0x80;
    case SF_ID:
        return (off & 1) ? 0xAA : 0xB0;   /* Sharp, LH28F016SU (as the 28F016SA family reports) */
    default:
        return f->data[off];
    }
}

void sf_write(sharp_flash *f, uint32_t off, uint8_t v)
{
    off &= SF_SIZE - 1;
    if (f->pending) {
        int cmd = f->pending;
        f->pending = 0;
        f->mode = SF_STATUS;
        switch (cmd) {
        case 0x40: case 0x10:                      /* byte write: bits can only be cleared */
            if (!f->vpp) { f->status |= 0x18; return; }
            f->data[off] &= v;
            if ((f->data[off] & v) != v) f->status |= 0x10;
            return;
        case 0x20:                                 /* block erase */
            if (v != 0xD0) { f->status |= 0x30; return; }
            if (!f->vpp) { f->status |= 0x28; return; }
            memset(f->data + (off & ~(SF_BLOCK - 1)), 0xFF, SF_BLOCK);
            return;
        case 0x30: case 0xA7:                      /* erase the whole chip */
            if (v != 0xD0) { f->status |= 0x30; return; }
            if (!f->vpp) { f->status |= 0x28; return; }
            memset(f->data, 0xFF, SF_SIZE);
            return;
        case 0x77:                                 /* lock a block: accepted, nothing locked */
            return;
        }
        return;
    }
    switch (v) {
    case 0xFF: f->mode = SF_ARRAY; break;
    case 0x70: f->mode = SF_STATUS; break;
    case 0x71: f->mode = SF_ESR; break;
    case 0x90: f->mode = SF_ID; break;
    case 0x50: f->status = 0x80; break;
    case 0x40: case 0x10: case 0x20: case 0x30: case 0xA7: case 0x77:
        f->pending = v;
        break;
    case 0xD0: case 0xB0: break;                   /* resume, suspend: nothing runs long enough to suspend */
    default:
        f->unknown++;
        f->last_unknown = v;
    }
}
