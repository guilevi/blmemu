/* cpu.h -- the CPU core's contract with its board: the Z180 (MAME's core, BSD-3-Clause, behind a step driver),
 * taken from ssi263-speech's Blazie emulator (MIT), where it is shared with that project's other cores.
 *
 * A board drives the core through this interface and never reaches into its internals; the core never knows which
 * board it is in.  The timing (what a step is, when interrupts are sampled, what HALT, SLP and EI do, and what cycle
 * count a callback sees) is ssi263-speech's src/csrc/cpu/CONTRACT.md.
 */
#ifndef SSI263_CPU_H
#define SSI263_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a core sees of its board.  *_create COPIES this struct; ctx must outlive the core.  Every callback gets ctx,
   the board's own instance: no globals anywhere, so two boards (two units) run side by side in one process, and on
   different threads.  Required: read, write, in, out.  Optional (NULL): fetch (-> read), irq_ack (-> FFh), serial_*,
   boundary.  A callback may call *_cycles, *_steps, *_pc, *_regs_get, *_set_irq and z180_csio_*; never *_step, *_run, *_reset or
   *_destroy.  CONTRACT.md 7 says what each callback sees, phase by phase. */
typedef struct cpu_bus {
    void *ctx;
    uint8_t (*read)(void *ctx, uint32_t addr);                /* memory: the PHYSICAL address (after any MMU) */
    uint8_t (*fetch)(void *ctx, uint32_t addr);               /* an opcode fetch; NULL: read */
    void (*write)(void *ctx, uint32_t addr, uint8_t value);
    uint8_t (*in)(void *ctx, uint16_t port);                  /* EXTERNAL I/O only: on-chip registers stay inside */
    void (*out)(void *ctx, uint16_t port, uint8_t value);
    /* the data bus during an interrupt acknowledge: byte n of the vector or, for the 8085's INTR, of the injected
       instruction (opcode, then operands); -1 = FFh */
    int (*irq_ack)(void *ctx, int line, int n);
    int (*serial_rx)(void *ctx, int channel);                 /* on-chip serial in: the next byte on the line, -1 = none */
    void (*serial_tx)(void *ctx, int channel, uint8_t byte);  /* on-chip serial out: a byte leaves the chip */
    int (*serial_pin)(void *ctx, int pin);                    /* input pins a core samples (Z180 /DCD0 /CTS; 8085 SID) */
    void (*serial_out_pin)(void *ctx, int pin, int level);    /* output pins (8085 SOD) */
    /* Phase D of every step (CONTRACT.md 1): after interrupt acceptance and its charge, after *_steps increments and
       the on-chip serial port has caught up; before the instruction.  The board's per-instruction work (scheduled
       keys, A/R readiness) happens here, so what it raises is sampled at the NEXT step's acceptance.  NULL: none. */
    void (*boundary)(void *ctx, uint32_t pc);
    /* x86 only: the host's software-interrupt seam (what Unicorn's UC_HOOK_INTR was; CONTRACT.md 4).  Called at E,
       from inside the instruction, for an INT n, INT 3, INTO taken (kind I86_INT_SOFTWARE) or a divide error
       (I86_INT_EXCEPTION), before anything is pushed; CS:IP are already past the instruction.  Nonzero: the host
       has serviced it -- nothing is pushed, the instruction ends there, and the registers are what the host left
       (it may call *_regs_set here); 0: the core vectors through the table as the CPU does.  Hardware interrupts
       and the single-step trap are never offered.  NULL: none. */
    int (*intercept)(void *ctx, int vector, int kind);
} cpu_bus;

/* ---- Z180 (Zilog Z80180 / Hitachi HD64180 family) ---------------------------------------------------------------- */
typedef struct z180 z180;

enum { Z180_INT0, Z180_INT1, Z180_INT2, Z180_NMI };            /* external lines; on-chip sources are internal */
enum { Z180_PIN_DCD0, Z180_PIN_CTS0 };                         /* bus->serial_pin: the ASCI's input pins */
#define Z180_DMA_CHUNK 16                                      /* burst DMA bytes per step (corrected path; model) */

typedef struct {                                               /* for lockstep tests: the programmer-visible state */
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc;
    uint8_t i, r, im, iff1, iff2, halted, sleeping;
} z180_regs;

z180 *z180_create(const cpu_bus *bus, double clock_hz);
void z180_destroy(z180 *c);
void z180_reset(z180 *c);
int z180_step(z180 *c);                            /* one step (CONTRACT.md 1); returns its T-states */
uint64_t z180_run(z180 *c, uint64_t budget);       /* whole steps until >= budget T-states; returns those run */
/* Today's cpu_execute_z180(budget), exactly, exceptions included (CONTRACT.md 3): NMI only at slice entry, a burst
   DMA chunk taking the rest of the budget, SLP ending the slice.  NOT equivalent to repeated steps.  For the current
   goldens only; retired with them. */
uint64_t z180_run_legacy(z180 *c, uint64_t budget);
void z180_set_irq(z180 *c, int line, int asserted);
uint64_t z180_cycles(const z180 *c);               /* T-states since reset (CONTRACT.md 5) */
uint8_t z180_ccr(const z180 *c);                   /* the Z8S180's CCR as written (bit 7: PHI = XTAL, not XTAL/2);
                                                      0 on a core without one (the legacy path) */
uint64_t z180_steps(const z180 *c);                /* steps since reset, HALT and SLP slots included */
uint32_t z180_pc(const z180 *c);                   /* the SAVED instruction-start PC (regs_get: architectural) */
void z180_regs_get(const z180 *c, z180_regs *out);

typedef struct {                                               /* an ASCI channel's control registers, as written */
    uint8_t cntla, cntlb, stat, asext;
    uint16_t astc;
} z180_asci_regs;
/* An ASCI channel's (0 or 1) control registers, read without side effects, as the core holds them (CONTRACT.md 9:
   the pin bits differ between cores).  A board whose host carries the serial line to a real port programs that
   port from them (the line format the firmware chose). */
void z180_asci_get(const z180 *c, int channel, z180_asci_regs *out);
/* The CSI/O (clocked serial I/O) driven by an EXTERNAL clock (CNTR's SS = 111): the device on the far end is the
   master and clocks 8 bits whenever it likes.  z180_csio_clock is those 8 clocks: when a transfer is armed (CNTR TE
   and/or RE), TRDR's byte goes out (into *sent, when TE) and `in` comes into TRDR (when RE); TE and RE clear and EF
   sets, which requests the CSI/O interrupt while EIE is set (a level: reading or writing TRDR clears EF).  Returns
   the CNTR bits that were armed (10h TE, 20h RE), 0 when none was (the clocks move nothing a program can see).  The
   Z180's own clock (SS < 111) is not modelled.  z180_csio_cntr: CNTR as the core holds it, no side effects.  Both
   may be called from bus->boundary. */
int z180_csio_clock(z180 *c, uint8_t in, uint8_t *sent);
uint8_t z180_csio_cntr(const z180 *c);

#ifdef __cplusplus
}
#endif
#endif
