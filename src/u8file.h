/* u8file.h -- files by UTF-8 path, on every system.  Windows' fopen, rename and the rest take the ANSI code page, so a
 * data folder under a user name such as "José" would not open; there the wide calls are used.  Header-only. */
#ifndef BLM_U8FILE_H
#define BLM_U8FILE_H

#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <wchar.h>

static inline int u8w(const char *s, wchar_t *w, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0;
}
static inline FILE *u8_fopen(const char *path, const char *mode)
{
    wchar_t wp[1024], wm[8];
    if (!u8w(path, wp, 1024) || !u8w(mode, wm, 8)) return NULL;
    return _wfopen(wp, wm);
}
static inline int u8_mkdir(const char *path)
{
    wchar_t wp[1024];
    return u8w(path, wp, 1024) ? _wmkdir(wp) : -1;
}
static inline int u8_remove(const char *path)
{
    wchar_t wp[1024];
    return u8w(path, wp, 1024) ? _wremove(wp) : -1;
}
static inline int u8_rename(const char *a, const char *b)
{
    wchar_t wa[1024], wb[1024];
    return u8w(a, wa, 1024) && u8w(b, wb, 1024) ? _wrename(wa, wb) : -1;
}
/* 0: exists; *dir set when it is a folder */
static inline int u8_stat(const char *path, int *dir)
{
    wchar_t wp[1024];
    struct _stat st;
    if (!u8w(path, wp, 1024) || _wstat(wp, &st)) return -1;
    if (dir) *dir = (st.st_mode & _S_IFDIR) != 0;
    return 0;
}
#else
static inline FILE *u8_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static inline int u8_mkdir(const char *path) { return mkdir(path, 0755); }
static inline int u8_remove(const char *path) { return remove(path); }
static inline int u8_rename(const char *a, const char *b) { return rename(a, b); }
static inline int u8_stat(const char *path, int *dir)
{
    struct stat st;
    if (stat(path, &st)) return -1;
    if (dir) *dir = S_ISDIR(st.st_mode);
    return 0;
}
#endif

#endif
