/* Synthetic programs exercise the board's public contract without private firmware. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blm_board.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static uint8_t program[512];
static size_t used;
static void byte(uint8_t value) { CHECK(used < sizeof program); program[used++] = value; }
static void load(uint8_t value) { byte(0x3e); byte(value); }
static void out(uint8_t port, uint8_t value) { load(value); byte(0xed); byte(0x39); byte(port); }
static void in(uint8_t port) { byte(0xed); byte(0x38); byte(port); }
static void store(uint8_t offset) { byte(0x32); byte(offset); byte(0x80); }
static void begin(void)
{
    memset(program, 0, sizeof program); used = 0;
    out(0x38, 0x78);                 /* common area 1: logical 8000h -> physical 80000h */
    out(0x3a, 0x80);
}
static blm_unit *machine(void)
{
    char err[256];
    blm_unit *u = blm_create(program, (long)sizeof program, NULL, err, sizeof err);
    CHECK(u != NULL);
    return u;
}
static void run(blm_unit *u, uint64_t cycles)
{
    char err[256];
    if (blm_run(u, cycles, err, sizeof err)) { fprintf(stderr, "%s\n", err); exit(1); }
}
static void power(blm_unit *u, int on)
{
    char err[256];
    if (blm_power(u, on, err, sizeof err)) { fprintf(stderr, "%s\n", err); exit(1); }
}
static void transfer(uint8_t value)
{
    out(0x0b, value); out(0x0a, 0x11);
    in(0x0a); byte(0xe6); byte(0x80); byte(0x28); byte(0xf9); /* wait for EF */
}
static void test_memory_and_fetch(void)
{
    blm_unit *u;
    FILE *stats;
    unsigned address, count;
    begin();
    load(0x5a); store(0);
    byte(0xdd); byte(0x21); byte(0x00); byte(0x80); /* LD IX,8000h */
    byte(0xdd); byte(0xcb); byte(0x00); byte(0x46); /* BIT 0,(IX+0) */
    byte(0x76);
    u = machine(); run(u, 1000);
    CHECK(blm_peek(u, 0x80000) == 0x5a);
    CHECK(blm_phys_pc(u) == used - 1);
    stats = tmpfile(); CHECK(stats != NULL);
    blm_fetch_stats(u, stats); rewind(stats);
    CHECK(fscanf(stats, "fetch %x %u", &address, &count) == 2);
    CHECK(address == 0 && count == 14); /* two OUT0 setup pairs, LD/store, DD, DDCB, HALT */
    fclose(stats); blm_destroy(u);
}
static void test_keyboard(void)
{
    blm_unit *u;
    unsigned i;
    begin();
    in(0x8c); store(0);
    for (i = 0; i < 8; i++) { out(0x8c, 0x80); out(0x8c, 0x82); }
    in(0x8c); store(1); byte(0x76);
    u = machine(); blm_keys(u, 1, 0); blm_keys(u, 1, 0);
    CHECK(blm_keys_pending(u) == 2); /* an unchanged held key is not queued twice */
    run(u, 2000);
    CHECK((blm_peek(u, 0x80000) & 0x20) == 0); /* first byte ready */
    CHECK((blm_peek(u, 0x80001) & 0x20) != 0); /* byte consumed, inter-byte gap */
    CHECK(blm_keys_pending(u) == 1);
    run(u, 7000); CHECK(blm_keys_pending(u) == 1);
    blm_destroy(u);
}
static void test_rtc(void)
{
    blm_unit *u;
    begin(); out(0xe0, 4);
    transfer(0x05);                  /* MSB-first A0h: write RTC register 20h */
    transfer(0x55);                  /* AAh */
    out(0xe0, 0); out(0xe0, 4);
    transfer(0x04);                  /* MSB-first 20h: read register 20h */
    in(0x0b);                       /* acknowledge the command's EF before receive */
    out(0x0a, 0x21);
    in(0x0a); byte(0xe6); byte(0x80); byte(0x28); byte(0xf9);
    in(0x0b); store(0);
    in(0x0a); byte(0xe6); byte(0x80); store(1);
    out(0xe0, 0); out(0x0a, 0x21);
    in(0x0a); byte(0xe6); byte(0x80); byte(0x28); byte(0xf9);
    in(0x0b); store(2); byte(0x76);
    u = machine(); run(u, 10000);
    CHECK(blm_peek(u, 0x80000) == 0x55);
    CHECK(blm_peek(u, 0x80001) == 0);
    CHECK(blm_peek(u, 0x80002) == 0xff);
    blm_destroy(u);
}
static void test_restart_clock(void)
{
    blm_unit *u;
    uint64_t before;
    begin(); byte(0x76);
    u = machine(); run(u, 1000); before = blm_cycles(u);
    power(u, 0); power(u, 1);
    CHECK(blm_cycles(u) == before);
    run(u, 1000); CHECK(blm_cycles(u) >= before + 1000);
    blm_destroy(u);
}
static void test_banked_memory_and_save(void)
{
    blm_unit *u, *restored;
    char err[256];
    const char *state = "board-test.state";
    begin();
    out(0x38, 0xd8);                 /* logical 8000h -> E0000h board window */
    out(0xf6, 1); out(0xf5, 4); load(0x11); store(0);
    out(0xf5, 5); load(0x22); store(0);
    out(0xf5, 4); byte(0x3a); byte(0); byte(0x80);
    byte(0x47);                      /* keep first bank's value in B while remapping */
    out(0x38, 0x78); byte(0x78); store(1);
    out(0x38, 0xd8); out(0xf5, 5); byte(0x3a); byte(0); byte(0x80); byte(0x47);
    out(0x38, 0x78); byte(0x78); store(2); byte(0x76);
    u = machine(); run(u, 2000);
    CHECK(blm_peek(u, 0x80001) == 0x11 && blm_peek(u, 0x80002) == 0x22);
    CHECK(blm_save(u, state) == 0);
    restored = machine(); CHECK(blm_load(restored, state) == 0);
    CHECK(blm_peek(restored, 0x80001) == 0x11 && blm_peek(restored, 0x80002) == 0x22);
    CHECK(blm_run(restored, 0, err, sizeof err) == 0 && blm_cycles(restored) == 0);
    CHECK(remove(state) == 0);
    blm_destroy(restored); blm_destroy(u);
}
static void test_creation_error(void)
{
    char err[256] = {0};
    CHECK(blm_create(NULL, 1, NULL, err, sizeof err) == NULL);
    CHECK(strstr(err, "empty program") != NULL);
}
int main(void)
{
    test_memory_and_fetch(); test_keyboard(); test_rtc(); test_restart_clock();
    test_banked_memory_and_save(); test_creation_error();
    puts("board tests passed");
    return 0;
}
