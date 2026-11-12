/* fwstore.h -- where the emulator keeps its things, and the firmware a person brings to it.
 *
 * Neither firmware ships with the emulator: the Millennium's program is Freedom Scientific's, the DoubleTalk's is RC
 * Systems'.  A person brings their own copies once; they are checked and copied into the data folder:
 *
 *   macOS    ~/Library/Application Support/blmemu
 *   Linux    $XDG_CONFIG_HOME/blmemu (~/.config/blmemu)
 *   Windows  %APPDATA%\blmemu
 *
 * holding firmware/BLM40ENU.BNS, firmware/rc8650.srec, the unit's memory (blm40.state) and the settings
 * (blmemu.ini).  BLMEMU_HOME overrides the folder (tests, a portable copy).
 *
 * The RC8650's firmware comes from RC Studio, RC Systems' free development software: its Bin folder holds each
 * chipset's firmware as two containers (the firmware and its BIOS), which fw_import_rc8650 joins into one S-record
 * image as librc8650's tools/rcstudio2srec.py does.  An .srec made that way is taken as it is.
 * Portable C.
 */
#ifndef BLM_FWSTORE_H
#define BLM_FWSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_PATH_MAX 1024

/* The data folder (created if need be), and paths inside it.  0, or -1 when it cannot be made. */
int fw_home(char *out, size_t cap);
int fw_path(const char *name, char *out, size_t cap);   /* fw_path("firmware/rc8650.srec", ...) */

#define FW_BNS_NAME "firmware/BLM40ENU.BNS"
#define FW_RC_NAME "firmware/rc8650.srec"
#define FW_STATE_NAME "blm40.state"
#define FW_INI_NAME "blmemu.ini"

/* What a Millennium update file is, from its contents. */
typedef struct {
    long image;                  /* where the program starts in the file */
    char revision[16];           /* "4.00", from its own "revision" string; "" if it has none */
    int known;                   /* its image is one this emulator was tried with */
} fw_bns_info;

/* 0: a Millennium 40 update; -1 with a reason a person can read. */
int fw_check_bns(const uint8_t *data, long n, fw_bns_info *info, char *err, size_t errlen);

/* Bring a file in: checked, then copied into the data folder.  0, or -1 with a reason.  `what` says what was
   taken ("Millennium 40 update, revision 4.00"; "RC8650 v2.60, model 4"). */
int fw_import_bns(const char *path, char *what, size_t whatlen, char *err, size_t errlen);
/* path: an .srec image, an RC Studio folder (or its Bin folder inside), or one of its m$*.bin containers (its
   partner is found beside it) */
int fw_import_rc8650(const char *path, char *what, size_t whatlen, char *err, size_t errlen);

/* Both firmwares are in the data folder. */
int fw_ready(void);

/* A path as a person typed or dropped it: quotes and a terminal's backslash escapes removed, ~ expanded, spaces at
   the ends dropped.  In place. */
void fw_clean_path(char *p);

/* A whole file by its UTF-8 path (malloc'd; NULL if it cannot be read). */
uint8_t *fw_read_all(const char *path, long *n);

void fw_sha256(const uint8_t *data, long n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
