/* host_getline.h -- getline for a build machine that has none.
 *
 * The m32c simulator's decoder generator (opc2c.c, GDB's) reads its input with POSIX getline, which Windows' C
 * runtime does not have, so a build on Windows itself (mingw-w64: w64devkit, MSYS2) stopped there.  prepare_m32c.sh
 * compiles opc2c with this file included first; everywhere else it adds nothing.
 */
#ifndef BLM_HOST_GETLINE_H
#define BLM_HOST_GETLINE_H
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>

static long long host_getline(char **line, size_t *cap, FILE *f)
{
    size_t n = 0;
    int c;
    if (!*line || !*cap) {
        *cap = 256;
        *line = (char *)realloc(*line, *cap);
        if (!*line) return -1;
    }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            char *more = (char *)realloc(*line, *cap * 2);
            if (!more) return -1;
            *line = more;
            *cap *= 2;
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = 0;
    return (long long)n;
}
#define getline host_getline
#endif
#endif
