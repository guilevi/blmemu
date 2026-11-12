/* fwstore.c -- see fwstore.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fwstore.h"
#include "../u8file.h"
#include "../blm_board.h"
#include "rc8650.h"
#ifdef _WIN32
#define SEP '\\'
#else
#define SEP '/'
#endif
#define MKDIR(p) u8_mkdir(p)

/* The Millennium 40 releases this emulator was run with: the sha256 of the image (the file from F3 C3 .. COPYRIGHT
   to its end). */
static const struct { const char *sha256, *what; } KNOWN_BNS[] = {
    {"977008186b158915bd3946e5b45e5ca360bc77eb46feb5bf8f5bb83b5eb303af", "BLM40ENU.BNS, revision 4.00 (February 2003)"},
};

/* ---- sha256 (FIPS 180-4) ----------------------------------------------------------------------------------------- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void fw_sha256(const uint8_t *data, long n, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t tail[128];
    uint64_t bits = (uint64_t)n * 8;
    long i, rest;
    int k, pad;
    for (i = 0; i + 64 <= n; i += 64) sha_block(h, data + i);
    rest = n - i;
    memcpy(tail, data + i, (size_t)rest);
    tail[rest] = 0x80;
    pad = rest < 56 ? 64 : 128;
    memset(tail + rest + 1, 0, (size_t)(pad - rest - 1));
    for (k = 0; k < 8; k++) tail[pad - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha_block(h, tail);
    if (pad == 128) sha_block(h, tail + 64);
    for (k = 0; k < 8; k++) {
        out[4 * k] = (uint8_t)(h[k] >> 24); out[4 * k + 1] = (uint8_t)(h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(h[k] >> 8); out[4 * k + 3] = (uint8_t)h[k];
    }
}

/* ---- the folder -------------------------------------------------------------------------------------------------- */
static void make_dirs(const char *path)            /* every folder along it */
{
    char p[FW_PATH_MAX];
    size_t i;
    snprintf(p, sizeof p, "%s", path);
    for (i = 1; p[i]; i++)
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = 0;
            MKDIR(p);
            p[i] = c;
        }
    MKDIR(p);
}

int fw_home(char *out, size_t cap)
{
    const char *o = getenv("BLMEMU_HOME"), *base;
#if defined(_WIN32)
    char ou[FW_PATH_MAX];                        /* in UTF-8 too, as every path here is, whatever the code page */
    const wchar_t *ow = _wgetenv(L"BLMEMU_HOME");
    o = ow && *ow && WideCharToMultiByte(CP_UTF8, 0, ow, -1, ou, (int)sizeof ou, NULL, NULL) ? ou : NULL;
#endif
    if (o && *o) snprintf(out, cap, "%s", o);
    else {
#if defined(_WIN32)
        {   /* the folder's name in UTF-8, whatever the code page */
            const wchar_t *w = _wgetenv(L"APPDATA");
            char u[FW_PATH_MAX];
            (void)base;
            if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, u, (int)sizeof u, NULL, NULL)) return -1;
            snprintf(out, cap, "%s\\blmemu", u);
        }
#elif defined(__APPLE__)
        base = getenv("HOME");
        if (!base) return -1;
        snprintf(out, cap, "%s/Library/Application Support/blmemu", base);
#else
        base = getenv("XDG_CONFIG_HOME");
        if (base && *base) snprintf(out, cap, "%s/blmemu", base);
        else {
            base = getenv("HOME");
            if (!base) return -1;
            snprintf(out, cap, "%s/.config/blmemu", base);
        }
#endif
    }
    make_dirs(out);
    return u8_stat(out, NULL) == 0 ? 0 : -1;
}

int fw_path(const char *name, char *out, size_t cap)
{
    char home[FW_PATH_MAX], *p;
    if (fw_home(home, sizeof home)) return -1;
    snprintf(out, cap, "%s%c%s", home, SEP, name);
    for (p = out + strlen(home); *p; p++)
        if (*p == '/' || *p == '\\') *p = SEP;
    return 0;
}

uint8_t *fw_read_all(const char *path, long *n)
{
    FILE *f = u8_fopen(path, "rb");
    uint8_t *d = NULL;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (*n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        d = (uint8_t *)malloc((size_t)*n);
        if (d && fread(d, 1, (size_t)*n, f) != (size_t)*n) { free(d); d = NULL; }
    }
    fclose(f);
    return d;
}

static int write_all(const char *name, const void *data, size_t n, char *err, size_t errlen)
{
    char path[FW_PATH_MAX], tmp[FW_PATH_MAX + 8], dir[FW_PATH_MAX], *s;
    FILE *f;
    if (fw_path(name, path, sizeof path)) { snprintf(err, errlen, "cannot make the data folder"); return -1; }
    snprintf(dir, sizeof dir, "%s", path);
    s = strrchr(dir, SEP);
    if (s) { *s = 0; make_dirs(dir); }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    f = u8_fopen(tmp, "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f)) {
        if (f) fclose(f);
        snprintf(err, errlen, "cannot write %s", path);
        return -1;
    }
    u8_remove(path);
    if (u8_rename(tmp, path)) { snprintf(err, errlen, "cannot write %s", path); return -1; }
    return 0;
}

int fw_ready(void)
{
    char p[FW_PATH_MAX];
    return fw_path(FW_BNS_NAME, p, sizeof p) == 0 && u8_stat(p, NULL) == 0 &&
           fw_path(FW_RC_NAME, p, sizeof p) == 0 && u8_stat(p, NULL) == 0;
}

void fw_clean_path(char *p)
{
    char tmp[FW_PATH_MAX];
    size_t i, j = 0, n = strlen(p);
    int q = 0;
    while (n && isspace((unsigned char)p[n - 1])) p[--n] = 0;
    for (i = 0; i < n && isspace((unsigned char)p[i]); i++) {}
    for (; i < n && j < sizeof tmp - 1; i++) {
        char c = p[i];
        if ((c == '\'' || c == '"') && (!q || q == c)) { q = q ? 0 : c; continue; }
#ifndef _WIN32
        if (c == '\\' && !q && i + 1 < n) { tmp[j++] = p[++i]; continue; }   /* a terminal's drag and drop */
#endif
        tmp[j++] = c;
    }
    tmp[j] = 0;
    if (tmp[0] == '~' && (tmp[1] == '/' || tmp[1] == 0) && getenv("HOME"))
        snprintf(p, FW_PATH_MAX, "%s%s", getenv("HOME"), tmp + 1);
    else
        snprintf(p, FW_PATH_MAX, "%s", tmp);
}

/* ---- the Millennium's update file ------------------------------------------------------------------------------ */
static const uint8_t *find(const uint8_t *h, long n, const char *needle)
{
    size_t m = strlen(needle);
    long i;
    for (i = 0; i + (long)m <= n; i++)
        if (memcmp(h + i, needle, m) == 0) return h + i;
    return NULL;
}

int fw_check_bns(const uint8_t *data, long n, fw_bns_info *info, char *err, size_t errlen)
{
    const uint8_t *r;
    uint8_t d[32];
    char hex[65];
    size_t k;
    memset(info, 0, sizeof *info);
    info->image = blm_find_image(data, n);
    if (info->image < 0) {
        snprintf(err, errlen, "this is not a Braille Lite program update (no program inside it)");
        return -1;
    }
    if (!find(data + info->image, n - info->image, "Millennium 40")) {
        snprintf(err, errlen, "this update is not for the Millennium 40 (BLM40ENU.BNS is)");
        return -1;
    }
    for (r = data + info->image; (r = find(r, (long)(data + n - r), "revision ")) != NULL; r++)
        if (r + 13 <= data + n && isdigit(r[9]) && r[10] == '.') {   /* "revision 4.00", not "revision number" */
            memcpy(info->revision, r + 9, 4);
            info->revision[4] = 0;
            break;
        }
    fw_sha256(data + info->image, n - info->image, d);
    for (k = 0; k < 32; k++) sprintf(hex + 2 * k, "%02x", d[k]);
    for (k = 0; k < sizeof KNOWN_BNS / sizeof *KNOWN_BNS; k++)
        if (!strcmp(hex, KNOWN_BNS[k].sha256)) info->known = 1;
    return 0;
}

int fw_import_bns(const char *path, char *what, size_t whatlen, char *err, size_t errlen)
{
    long n = 0;
    fw_bns_info info;
    uint8_t *data = fw_read_all(path, &n);
    int r;
    if (!data) { snprintf(err, errlen, "cannot read %s", path); return -1; }
    if (fw_check_bns(data, n, &info, err, errlen)) { free(data); return -1; }
    r = write_all(FW_BNS_NAME, data, (size_t)n, err, errlen);
    free(data);
    if (r) return -1;
    snprintf(what, whatlen, "Millennium 40 update%s%s%s", info.revision[0] ? ", revision " : "", info.revision,
             info.known ? "" : " (not one this emulator was tried with: it may not run)");
    return 0;
}

/* ---- the RC8650's firmware ---------------------------------------------------------------------------------------- */
/* RC Studio's firmware/BIOS pairs, as it pairs them (librc8650's tools/rcstudio2srec.py), the RC8650's in the order
   they are preferred: model 4 is librc8650's reference image. */
static const char *const PAIRS[][2] = {
    {"m$4a95b2", "m$4f8c6a"},   /* RC8650 v2.60, model 4 */
    {"m$195a22", "m$11f9aa"},   /* model 1 */
    {"m$4b39ca", "m$4f92e7"},   /* model 7 */
    {"m$28a5f8", "m$2a11b2"},   /* model 2 */
    {"m$29c1d3", "m$2b71e1"},   /* model 8 */
};
#define N_PAIRS (int)(sizeof PAIRS / sizeof *PAIRS)

/* a container: u32 address, u32 length, the payload as a running XOR (seed 80h), u16 sum of the rest */
static uint8_t *container(const char *path, uint32_t *addr, long *len, char *err, size_t errlen)
{
    long n = 0, i;
    uint8_t *b = fw_read_all(path, &n), *plain, acc = 0x80;
    uint32_t sum = 0, length;
    if (!b) { snprintf(err, errlen, "cannot read %s", path); return NULL; }
    if (n < 10) goto bad;
    *addr = ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24) & 0xFFFFF;
    length = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    if ((long)length + 10 != n) goto bad;
    for (i = 0; i < 8 + (long)length; i++) sum += b[i];
    if ((sum & 0xFFFF) != ((uint32_t)b[8 + length] | (uint32_t)b[9 + length] << 8)) goto bad;
    plain = (uint8_t *)malloc(length ? length : 1);
    if (!plain) goto bad;
    for (i = 0; i < (long)length; i++) plain[i] = acc ^= b[8 + i];
    *len = (long)length;
    free(b);
    return plain;
bad:
    free(b);
    snprintf(err, errlen, "%s is not an RC Studio firmware file", path);
    return NULL;
}

static size_t srec_line(char *out, char type, uint32_t addr, int abytes, const uint8_t *d, int n)
{
    uint8_t body[64];
    int count = abytes + n + 1, i, sum;
    size_t k = 0;
    for (i = 0; i < abytes; i++) body[i] = (uint8_t)(addr >> (8 * (abytes - 1 - i)));
    memcpy(body + abytes, d, (size_t)n);
    sum = count;
    k += (size_t)sprintf(out + k, "S%c%02X", type, count);
    for (i = 0; i < abytes + n; i++) { k += (size_t)sprintf(out + k, "%02X", body[i]); sum += body[i]; }
    k += (size_t)sprintf(out + k, "%02X\n", ~sum & 0xFF);
    return k;
}

static char *to_srec(const uint8_t *fw, long fwn, uint32_t fwa, const uint8_t *bios, long bn, uint32_t ba,
                     size_t *outn)
{
    size_t cap = (size_t)(fwn + bn) * 3 + 4096, k = 0;
    char *s = (char *)malloc(cap);
    long o;
    if (!s) return NULL;
    k += srec_line(s + k, '0', 0, 2, (const uint8_t *)"HDR", 3);
    for (o = 0; o < fwn; o += 32) k += srec_line(s + k, '3', fwa + (uint32_t)o, 4, fw + o, fwn - o < 32 ? (int)(fwn - o) : 32);
    for (o = 0; o < bn; o += 32) k += srec_line(s + k, '3', ba + (uint32_t)o, 4, bios + o, bn - o < 32 ? (int)(bn - o) : 32);
    k += srec_line(s + k, '7', 0, 4, NULL, 0);
    *outn = k;
    return s;
}

static int file_exists(const char *p)
{
    int dir;
    return u8_stat(p, &dir) == 0 && !dir;
}

static int is_dir(const char *p)
{
    int dir;
    return u8_stat(p, &dir) == 0 && dir;
}

static int describe(const void *srec, size_t n, char *what, size_t whatlen, char *err, size_t errlen)
{
    rc8650_profile p;
    if (rc8650_image_info(srec, n, &p) != 0) {
        snprintf(err, errlen, "this is not an RC8650 firmware image");
        return -1;
    }
    if (p.chip != 8650) {
        snprintf(err, errlen, "this firmware is for the RC%u, not the RC8650 the Millennium has", p.chip);
        return -1;
    }
    snprintf(what, whatlen, "RC8650 v%x.%02x, model %u", p.version >> 8, p.version & 0xFF, p.model);
    return 0;
}

static int import_pair(const char *fwp, const char *biosp, char *what, size_t whatlen, char *err, size_t errlen)
{
    uint32_t fa, ba;
    long fn, bn;
    size_t sn;
    uint8_t *fw = container(fwp, &fa, &fn, err, errlen), *bios;
    char *s;
    int r;
    if (!fw) return -1;
    bios = container(biosp, &ba, &bn, err, errlen);
    if (!bios) { free(fw); return -1; }
    if (fn < 13 || fw[0] != 0xFE) {
        snprintf(err, errlen, "%s has no firmware header", fwp);
        free(fw); free(bios);
        return -1;
    }
    fw[8] = fw[9] = 0;                 /* the validity mark RC Studio's loader writes last, once programmed */
    s = to_srec(fw, fn, fa, bios, bn, ba, &sn);
    free(fw);
    free(bios);
    if (!s) { snprintf(err, errlen, "out of memory"); return -1; }
    r = describe(s, sn, what, whatlen, err, errlen) || write_all(FW_RC_NAME, s, sn, err, errlen) ? -1 : 0;
    free(s);
    return r;
}

int fw_import_rc8650(const char *path, char *what, size_t whatlen, char *err, size_t errlen)
{
    char dir[FW_PATH_MAX], a[FW_PATH_MAX + 32], b[FW_PATH_MAX + 32];
    int i, k;
    if (is_dir(path)) {
        const char *const subs[] = {"", "Bin", "RCStudio/Bin", "RCStudio\\Bin"};
        for (k = 0; k < 4; k++) {
            snprintf(dir, sizeof dir, "%s%s%s", path, subs[k][0] ? "/" : "", subs[k]);
            for (i = 0; i < N_PAIRS; i++) {
                snprintf(a, sizeof a, "%s/%s.bin", dir, PAIRS[i][0]);
                snprintf(b, sizeof b, "%s/%s.bin", dir, PAIRS[i][1]);
                if (file_exists(a) && file_exists(b)) return import_pair(a, b, what, whatlen, err, errlen);
            }
        }
        snprintf(err, errlen, "no RC8650 firmware in that folder (RC Studio keeps it in its Bin folder)");
        return -1;
    }
    {
        const char *base = strrchr(path, '/'), *bs = strrchr(path, '\\');
        if (bs && (!base || bs > base)) base = bs;
        base = base ? base + 1 : path;
        for (i = 0; i < N_PAIRS; i++)
            for (k = 0; k < 2; k++)
                if (!strncmp(base, PAIRS[i][k], strlen(PAIRS[i][k]))) {   /* one container: its partner beside it */
                    snprintf(dir, sizeof dir, "%.*s", (int)(base - path), path);
                    snprintf(a, sizeof a, "%s%s.bin", dir, PAIRS[i][0]);
                    snprintf(b, sizeof b, "%s%s.bin", dir, PAIRS[i][1]);
                    if (!file_exists(a) || !file_exists(b)) {
                        snprintf(err, errlen, "%s needs %s.bin beside it", base, PAIRS[i][!k]);
                        return -1;
                    }
                    return import_pair(a, b, what, whatlen, err, errlen);
                }
    }
    {
        long n = 0;
        uint8_t *d = fw_read_all(path, &n);
        int r;
        if (!d) { snprintf(err, errlen, "cannot read %s", path); return -1; }
        r = describe(d, (size_t)n, what, whatlen, err, errlen) || write_all(FW_RC_NAME, d, (size_t)n, err, errlen) ? -1 : 0;
        free(d);
        return r;
    }
}
