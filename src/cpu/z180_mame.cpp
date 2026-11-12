// z180_mame.cpp -- cpu.h's Z180 on MAME's Z180 core: the CORRECTED path (CONTRACT.md 1-2, 4-7, 9).
//
// One translation unit: MAME's code (z180_mame_machine.cpp, which includes the vendored instruction files) is
// included here, because its register macros (_PCD, PUSH, ...) and inline helpers (ROP, RM, ...) must be seen by
// the step driver and should inline into it.  Build this file and z180_asci.cpp; never z180_mame_machine.cpp alone.
//
// The driver is ours.  It follows MAME's execute_run (z180.cpp, the revision in mame_z180/PINNED.txt) piece by
// piece, one step at a time, in the contract's phases:
//   A  NMI (edge) first, as execute_run's entry check but at every step; else INT0 in IM0 (injected: nothing
//      pushed or read here); else the maskable sources through MAME's check_interrupts (IFF1 and the EI shadow
//      gate them).  A burst-DMA step accepts only the NMI, which stops DMA.  z180_pc() is the interrupted address.
//   B  the acceptance T-states, and the on-chip timers clocked by them (handle_io_timers).
//   C  the EI shadow ends.
//   D  steps++, the saved PC, the ASCI catch-up, bus->boundary.
//   E  one instruction (the injected one after an IM0 acceptance), or one HALT/SLP slot of 3 T-states (as
//      execute_run), or one burst-DMA chunk.
//   F  its T-states, and the timers clocked by them.
//   G  cycle-stolen DMA as execute_run does it after each instruction (channel 0, one transfer, then channel 1),
//      only while DME is set, and none in SLEEP as it stands after E (Zilog: SLEEP stops the DMAC).
//
// Differences from MAME, each deliberate:
//   - NMI is sampled at every step (MAME: at execute_run's entry only; the legacy core keeps that quirk).
//   - Burst DMA runs in fixed chunks of Z180_DMA_CHUNK bytes, one chunk per step (MAME: until the slice's budget
//     runs out).  z180_dma0(0) moves exactly one byte: its loop stops once the cycles spent exceed the budget.
//   - SLP: an individually enabled interrupt request ends SLEEP even with IEF1 = 0, and the CPU continues after SLP
//     without vectoring (Zilog Z8018x UM, SLEEP).  MAME keeps the CPU asleep until a request is accepted.  HALT is
//     unchanged: a request masked by IEF1 leaves it halted.
//   - IOSTOP (ICR bit 5) stops the PRT/FRC and the ASCI (Zilog UM); MAME clocks them regardless.
//   - The ASCI is ours (z180_asci.hpp); its interrupt request is a level.
//   - IM0: the acknowledged instruction is injected at E (drv_injected_instruction), operands from further
//     acknowledge bytes; MAME's take_interrupt pushes for any byte but CALL and JP and reads one byte.
//   - SLEEP stops the DMAC, including in the step whose instruction is the SLP; HALT does not.
//   - TRAP: MAME has none (it logs an undefined opcode and runs the Z80's form).  Here the prefixes are dispatched
//     by drv_instruction, which TRAPs every opcode the Z180's op code maps leave undefined (z180_trap.hpp), and
//     software can clear ITC.TRAP but not set it (the extraction's ITC substitution).
// Known limits: an undefined opcode injected by an IM0 acknowledge does not TRAP (the manual says it should; no
// board injects one), and a prefixed injected opcode's PC is not held.  Memory and I/O wait states are MAME's:
// DCNTL's MWI/IWI are charged on every access (the legacy core charged them only in DMA).  Every one of these goes
// into the comparison with the legacy core before new goldens.
#include "z180_mame_machine.cpp"
#include "z180_trap.hpp"

#include <new>

// ---- the driver's pieces, as members (MAME's macros name members) ----------------------------------------------

bool z180_device::drv_burst() const
{
    return m_HALT != 2                    // SLEEP stops the DMAC (Zilog UM); HALT does not
           && (m_dstat & Z180_DSTAT_DME) && (m_dstat & Z180_DSTAT_DE0)
           && (m_dmode & Z180_DMODE_MMOD) == Z180_DMODE_MMOD;
}

// The length of an unprefixed opcode (its operand bytes follow it).  For an injected instruction the PC must not
// move, so it is set back by the operand count before the operands are read; ROP/ARG then bring it to where it was.
static int z180_op_length(uint8_t op)
{
    switch (op) {
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x22: case 0x2a: case 0x32: case 0x3a:
    case 0xc2: case 0xc3: case 0xc4: case 0xca: case 0xcc: case 0xcd: case 0xd2: case 0xd4: case 0xda: case 0xdc:
    case 0xe2: case 0xe4: case 0xea: case 0xec: case 0xf2: case 0xf4: case 0xfa: case 0xfc:
        return 3;
    case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x26: case 0x2e: case 0x36: case 0x3e:
    case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xc6: case 0xce: case 0xd6: case 0xde: case 0xe6: case 0xee: case 0xf6: case 0xfe: case 0xd3: case 0xdb:
        return 2;
    default:
        return 1;
    }
}

// Phase E after an IM0 acceptance (CONTRACT.md 4): the step's one instruction comes from the acknowledge -- byte 0
// the opcode, then any operands and further op codes -- and does its own control transfer: an RST or a CALL pushes
// the interrupted PC as that instruction does; a NOP pushes nothing.  No ordinary instruction follows in this step.
// CHANGED from MAME, whose take_interrupt pushes for any byte but CALL and JP and reads only one acknowledge byte.
//
// The PC is held: an unprefixed opcode's operands are known from its length, so the PC is set back by them first
// (an RST or CALL then pushes the interrupted address); a prefixed one never pushes the PC, so it is restored after,
// unless the instruction moved it (JP (IX), RETN/RETI).  An undefined prefixed form TRAPs (the manual: "if an
// invalid instruction is fetched during Mode 0 interrupt acknowledge"), stacking the interrupted PC (model).
//
// T-states (Zilog UM, printed page 76, Figure 36: the INT0 acknowledge cycle is T1 T2 TW* TW* T3, the two wait
// states automatic): the instruction's own, with its 3-T opcode fetch replaced by that 5-T cycle -- an RST is
// 5 + 2 Ti + 6 (push) = 13, as the figure.  Acknowledge bytes take no programmed memory waits (the fetch helpers'
// charges for them are taken back); the instruction's real memory accesses (the pushes) keep theirs.
// R counts the acknowledge's M1 cycle ("R increments for each CPU Op Code fetch cycle (each M1 cycle)", page 177),
// and the prefixed forms' further op code fetches as ordinary fetches do.
int z180_device::drv_injected_instruction()
{
    m_inject.n = 0;
    m_inject.on = 1;
    m_inject_pc0 = _PCD;
    m_R++;                                // the acknowledge cycle is an M1 cycle
    m_extra_cycles = 0;
    uint8_t op = z180_ack_byte(m_bus, &m_inject);
    int t;
    if (op == 0xcb || op == 0xdd || op == 0xed || op == 0xfd) {
        t = drv_dispatch(op);
        // Decided by the instruction, not by where the PC ended up (a jump to the interrupted PC + 1 is
        // arithmetically the fetches' own advance).  The prefixed forms that set the PC are JP (IX),
        // JP (IY), RETN and RETI; for every other one -- the repeating block instructions included, which run once
        // here (model: their "repeat" would re-fetch from memory, not from the acknowledge) -- the PC is put back.
        uint8_t b2 = m_inject.bytes[1];
        bool transfer = ((op == 0xdd || op == 0xfd) && b2 == 0xe9) || (op == 0xed && (b2 == 0x45 || b2 == 0x4d));
        if (!m_inject_trapped && !transfer)
            _PCD = m_inject_pc0;          // the PC is where the interrupt found it
    } else {
        _PCD = (_PCD - (z180_op_length(op) - 1)) & 0xffff;
        t = exec_op(op) + m_extra_cycles;
    }
    t += 5 - 3;                           // the acknowledge cycle in place of the opcode fetch
    t -= (m_inject.n - 1) * memory_wait_states();   // acknowledge bytes 1.. were charged memory waits by ROP/ARG
    m_inject.on = 0;
    m_inject_trapped = 0;
    return t;
}

bool z180_device::drv_sleep_wake_request()
{
    if (m_irq_state[0] != CLEAR_LINE && (m_itc & Z180_ITC_ITE0)) return true;
    if (m_irq_state[1] != CLEAR_LINE && (m_itc & Z180_ITC_ITE1)) return true;
    if (m_irq_state[2] != CLEAR_LINE && (m_itc & Z180_ITC_ITE2)) return true;
    if ((m_tcr & Z180_TCR_TIE0) && (m_tcr & Z180_TCR_TIF0)) return true;
    if ((m_tcr & Z180_TCR_TIE1) && (m_tcr & Z180_TCR_TIF1)) return true;
    if (m_int_pending[Z180_INT_DMA0] || m_int_pending[Z180_INT_DMA1]) return true;
    return m_csio->check_interrupt() || m_asci[0]->check_interrupt() || m_asci[1]->check_interrupt();
}

int z180_device::drv_accept(bool burst)
{
    if (m_nmi_pending) {                  // execute_run's NMI entry, as upstream
        LEAVE_HALT();
        m_dstat &= ~Z180_DSTAT_DME;       // NMI disables DMA transfers
        m_IFF2 = m_IFF1;
        m_IFF1 = 0;
        PUSH( PC );
        _PCD = 0x0066;
        m_nmi_pending = 0;
        return 11;
    }
    if (burst)
        return 0;                         // the DMAC has the bus
    // INT0 in IM0 is an injected instruction: accepted here (IFF1 and IFF2 cleared, HALT left), nothing pushed or
    // read; E runs it.  INT0 is the highest maskable source, so check_interrupts would have taken it first.
    if (m_IM == 0 && m_IFF1 && !m_after_EI && m_irq_state[0] != CLEAR_LINE && (m_itc & Z180_ITC_ITE0)) {
        LEAVE_HALT();
        m_IFF1 = m_IFF2 = 0;
        m_inject_pending = 1;
        return 0;
    }
    int t = check_interrupts();
    if (t == 0 && m_HALT == 2 && !m_IFF1 && drv_sleep_wake_request())
        LEAVE_HALT();                     // CHANGED: SLEEP ends without service; on after SLP
    return t;
}

int z180_device::drv_instruction()
{
    if (m_inject_pending) {
        m_inject_pending = 0;
        return drv_injected_instruction();
    }
    if (m_HALT)
        return 3;                         // a HALT or SLP slot (execute_run: 3 T-states)
    _PPC = _PCD;
    m_R++;
    m_extra_cycles = 0;
    return drv_dispatch(ROP());
}

// CHANGED: the prefixes are dispatched here, as MAME's op_cb/op_dd/op_ed/op_fd and dd_cb/fd_cb do (their R
// increments and cycle-table terms included), so that an opcode the Z180 does not define (z180_trap.hpp) TRAPs at
// its fetch instead of running MAME's Z80 form.  R counts each op code fetch (M1) as it happens, a trapped one
// too (page 177).  `op` has been fetched (and counted) by the caller.  During an injected instruction the fetches
// read the acknowledge (z180_inject).
int z180_device::drv_dispatch(uint8_t op)
{
    switch (op) {
    case 0xcb: {
        uint8_t b2 = ROP();
        m_R++;
        if (!z180_trap::cb_defined(b2))
            return drv_trap(false);
        m_extra_cycles += exec_cb(b2);
        return m_cc[Z180_TABLE_op][op] + m_extra_cycles;
    }
    case 0xed: {
        uint8_t b2 = ROP();
        m_R++;
        if (!z180_trap::ed_defined(b2))
            return drv_trap(false);
        m_extra_cycles += exec_ed(b2);
        return m_cc[Z180_TABLE_op][op] + m_extra_cycles;
    }
    case 0xdd:
    case 0xfd: {
        uint8_t b2 = ROP();
        m_R++;
        if (!z180_trap::xy_defined(b2))
            return drv_trap(false);
        if (b2 != 0xcb) {
            m_extra_cycles += op == 0xdd ? exec_dd(b2) : exec_fd(b2);
            // CHANGED: every defined DD/FD body adds its own m_R++ on top of op_dd's (checked: one in each), so
            // MAME counts DD xx three times; the manual counts op code fetches (M1): DD and xx, two
            m_R--;
            return m_cc[Z180_TABLE_op][op] + m_extra_cycles;
        }
        m_R++;                            // dd_cb's: DD, CB and the 4th byte are op code fetches (Figure 33), three
        uint8_t d = ARG();
        uint8_t b4 = ROP();
        m_ea = (uint32_t)(uint16_t)((op == 0xdd ? _IX : _IY) + (int8_t)d);   // EAX() / EAY()
        if (!z180_trap::xycb_defined(b4)) {
            RM(m_ea);                     // Figure 33: the memory read at IX+d / IY+d comes before the stacking
            return drv_trap(true);
        }
        m_extra_cycles += exec_xycb(b4);
        return m_cc[Z180_TABLE_op][op] + m_cc[Z180_TABLE_xy][0xcb] + m_extra_cycles;
    }
    default:
        return exec_op(op) + m_extra_cycles;
    }
}

// TRAP (Zilog UM, printed pages 70-72): ITC.TRAP set; UFO = 1 when the undefined byte was the third op code
// (DDCB/FDCB), else 0; the PC is stacked so that the instruction starts at the stacked PC - 1 (UFO 0) or - 2
// (UFO 1); execution restarts at logical 0000h.  IEF1/IEF2 are not affected (the manual's Table 8); TRAP is not
// maskable.  In the contract's phases it is the step's own instruction: found at E, charged at F.
//   Figure 32 (2nd op code): fetches 3 + 3, six states (TTP and five Ti), stacking 3 + 3 = 18 T.
//   Figure 33 (3rd op code): fetches DD, CB, d, op 3 each, the read at IX+d 4 (T1 T2 TTP T3), four states,
//   stacking 6 = 26 T.  Plus the programmed memory waits of every fetch, read and write (charged by the helpers).
// The stack is written as the figures show: PCH to SP-1 first, then PCL to SP-2.  (Elsewhere -- CALL, RST, the
// interrupt pushes -- MAME's PUSH writes the low byte first: the bytes are right, the bus order is not; a known
// approximation.)  An injected undefined instruction stacks the PC the interrupt found (model).
int z180_device::drv_trap(bool ufo)
{
    m_itc = (uint8_t)((m_itc | Z180_ITC_TRAP) & ~Z180_ITC_UFO) | (ufo ? Z180_ITC_UFO : 0);
    uint16_t stacked = m_inject.on ? (uint16_t)m_inject_pc0 : (uint16_t)((_PCD - (ufo ? 2 : 1)) & 0xffff);
    if (m_inject.on)
        m_inject_trapped = 1;
    _SP -= 2;
    WM((_SPD + 1) & 0xffff, (uint8_t)(stacked >> 8));
    WM(_SPD, (uint8_t)stacked);
    _PCD = 0x0000;
    return (ufo ? 3 * 4 + 4 + 4 + 6 : 3 * 2 + 6 + 6) + m_extra_cycles;
}

int z180_device::drv_burst_chunk()
{
    int t = 0;
    for (int n = 0; n < Z180_DMA_CHUNK && drv_burst(); n++)
        t += z180_dma0(0);                // one byte per call
    return t;
}

// ---- cpu.h ------------------------------------------------------------------------------------------------------

struct z180 {
    cpu_bus bus;
    z8s180_device *dev;
    uint64_t cycles, steps;
    uint32_t pc;
    int line[4];                          // the external lines as last set (Z180_INT0..Z180_NMI), kept over reset
};

namespace {

inline void charge(z180 *c, int t)       // T-states pass: counted, and the PRT/FRC clocked unless IOSTOP
{
    if (t <= 0)
        return;
    c->cycles += (uint64_t)t;
    if (!(c->dev->m_iocr & Z180_IOCR_IOSTP))
        c->dev->handle_io_timers(t);
}

void apply_lines(z180 *c)                // after a reset: level lines re-sampled, the NMI edge not re-armed
{
    for (int k = 0; k < 3; k++)
        c->dev->execute_set_input(Z180_INPUT_LINE_IRQ0 + k, c->line[k] ? ASSERT_LINE : CLEAR_LINE);
    c->dev->m_nmi_state = c->line[Z180_NMI] ? ASSERT_LINE : CLEAR_LINE;
    c->dev->m_nmi_pending = 0;
}

}  // namespace

extern "C" {

z180 *z180_create(const cpu_bus *bus, double clock_hz)
{
    (void)clock_hz;                       // the core counts T-states; the board turns them into time
    z180 *c = new (std::nothrow) z180();
    if (!c)
        return nullptr;
    c->bus = *bus;
    c->dev = new (std::nothrow) z8s180_device(&c->bus);
    if (!c->dev) {
        delete c;
        return nullptr;
    }
    c->dev->init_tables();                // allocates and fills this instance's flag tables
    z180_reset(c);
    return c;
}

void z180_destroy(z180 *c)
{
    if (!c)
        return;
    delete c->dev;
    delete c;
}

void z180_reset(z180 *c)
{
    c->dev->device_reset();
    c->dev->m_asci_0.reset();
    c->dev->m_asci_1.reset();
    c->dev->m_csio_0.reset();
    c->dev->m_inject_pending = 0;
    c->dev->m_inject.on = 0;
    c->cycles = c->steps = 0;
    c->pc = 0;
    apply_lines(c);
}

int z180_step(z180 *c)
{
    z180_device &d = *c->dev;
    bool burst = d.drv_burst();

    // A's callbacks (the stack writes, a vector read) see the interrupted instruction's address: where execution
    // resumes, which is what acceptance pushes.  A halted core's PC rests on its HALT (or on SLP's first byte), so
    // that address is past it.  D then sets the saved instruction-start PC as usual.
    c->pc = (uint32_t)((d.m_PC.w.l + (d.m_HALT == 2 ? 2 : d.m_HALT ? 1 : 0)) & 0xffff);
    int acc = d.drv_accept(burst);        // A
    charge(c, acc);                       // B
    d.m_after_EI = 0;                     // C
    if (acc && burst)
        burst = d.drv_burst();            // an NMI stopped the DMA: this step runs an instruction

    c->steps++;                           // D
    c->pc = d.m_PC.w.l;
    if (d.m_iocr & Z180_IOCR_IOSTP) {
        d.m_asci_0.hold(c->cycles);
        d.m_asci_1.hold(c->cycles);
    } else {
        d.m_asci_0.catch_up(c->cycles);
        d.m_asci_1.catch_up(c->cycles);
    }
    if (c->bus.boundary)
        c->bus.boundary(c->bus.ctx, c->pc);

    int ins = burst ? d.drv_burst_chunk() : d.drv_instruction();   // E
    charge(c, ins);                       // F

    // G: not in SLEEP as it stands now (an SLP just executed stops the DMAC too), and not when burst mode is on
    // now (execute_run re-checks it first: the next step is a chunk).  A HALT leaves the DMAC running.
    int dma = 0;
    if (!burst && d.m_HALT != 2 && (d.m_dstat & Z180_DSTAT_DME) && !d.drv_burst()) {
        int t = d.z180_dma0(6);
        charge(c, t);
        dma += t;
        t = d.z180_dma1();
        charge(c, t);
        dma += t;
    }
    return acc + ins + dma;
}

uint64_t z180_run(z180 *c, uint64_t budget)
{
    uint64_t done = 0;
    while (done < budget)
        done += (uint64_t)z180_step(c);
    return done;
}

void z180_set_irq(z180 *c, int line, int asserted)
{
    if (line < Z180_INT0 || line > Z180_NMI)
        return;
    c->line[line] = asserted ? 1 : 0;
    // cpu.h's lines are not MAME's input numbers: Z180_NMI (3) is MAME's DREQ0
    c->dev->execute_set_input(line == Z180_NMI ? INPUT_LINE_NMI : Z180_INPUT_LINE_IRQ0 + line,
                              asserted ? ASSERT_LINE : CLEAR_LINE);
}

void z180_asci_get(const z180 *c, int channel, z180_asci_regs *out)
{
    const z180_asci &a = channel ? c->dev->m_asci_1 : c->dev->m_asci_0;
    out->cntla = a.cntla_r();
    out->cntlb = a.cntlb_r();             // bit 5 (/CTS) reads 0 here: the written value is not kept
    out->stat = a.stat_r();
    out->asext = a.asext_r();
    out->astc = (uint16_t)(a.astcl_r() | (a.astch_r() << 8));
}

int z180_csio_clock(z180 *c, uint8_t in, uint8_t *sent) { return c->dev->m_csio_0.clock(in, sent); }
uint8_t z180_csio_cntr(const z180 *c) { return c->dev->m_csio_0.cntr_r(); }

uint64_t z180_cycles(const z180 *c) { return c->cycles; }
uint8_t z180_ccr(const z180 *c) { return c->dev->m_ccr; }
uint64_t z180_steps(const z180 *c) { return c->steps; }
uint32_t z180_pc(const z180 *c) { return c->pc; }

void z180_regs_get(const z180 *c, z180_regs *out)
{
    const z180_device &d = *c->dev;
    out->af = d.m_AF.w.l; out->bc = d.m_BC.w.l; out->de = d.m_DE.w.l; out->hl = d.m_HL.w.l;
    out->af2 = d.m_AF2.w.l; out->bc2 = d.m_BC2.w.l; out->de2 = d.m_DE2.w.l; out->hl2 = d.m_HL2.w.l;
    out->ix = d.m_IX.w.l; out->iy = d.m_IY.w.l; out->sp = d.m_SP.w.l; out->pc = d.m_PC.w.l;
    out->i = d.m_I; out->r = (uint8_t)((d.m_R & 0x7f) | (d.m_R2 & 0x80)); out->im = d.m_IM;
    out->iff1 = d.m_IFF1; out->iff2 = d.m_IFF2; out->halted = d.m_HALT ? 1 : 0; out->sleeping = d.m_HALT == 2;
}

}  // extern "C"
