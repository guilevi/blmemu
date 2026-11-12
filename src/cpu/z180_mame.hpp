// z180_mame.hpp -- the class MAME's Z180 code compiles against, without the MAME framework.
//
// MAME's instruction files (mame_z180/, unchanged) and machine-level code (z180_mame_machine.cpp, generated from
// z180.cpp by extract_z180_machine.py) are written as members of `z180_device`.  This header declares that class
// with the member names they use -- registers, internal registers, the flag tables, the opcode functions -- taken
// from MAME's z180.h at the revision in mame_z180/PINNED.txt, and replaces what the framework provided:
//
//   MAME                                   here
//   memory_access<...> m_program etc.      z180_mem / z180_io: the cpu_bus callbacks, with its ctx
//   devcb_write_line m_tend0_cb            a no-op (the Braille Lite does not wire /TEND)
//   required_device m_asci[2], m_csio      z180_asci, z180_csio (z180_asci.hpp: our byte-level serial ports)
//   z80 daisy chain                        none: the IM0/IM2 vector comes from cpu_bus.irq_ack
//   standard_irq_callback                  cpu_bus.irq_ack(ctx, Z180_INT0, 0); -1 or none = FFh
//   LOG / logerror                         discarded
//   static flag tables (z180.cpp)          members, so two instances share nothing (CONTRACT.md 9)
//   device_start                           init_tables(): its flag-table part
//
// The class is the Z8S180 (z8s180_device, as upstream): the Braille Lite firmware probes CCR at start-up and takes
// the Z8S180 path when it reads back, as it does on the legacy core.  z180_mame.cpp holds our step driver and the
// cpu.h API.  Licence: this header is ours (MIT) but restates MAME's declarations (BSD-3-Clause, copyright-holders:
// Juergen Buchmueller; see mame_z180/LICENSE-BSD-3-Clause.txt).
#ifndef SSI263_Z180_MAME_HPP
#define SSI263_Z180_MAME_HPP

#include <cstdint>
#include <cstring>
#include <memory>

#include "cpu.h"
#include "z180_asci.hpp"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "PAIR below is laid out for a little-endian host, as MAME's is on one"
#endif

// ---- MAME's basic types (emu/emucore.h, lib/util), little-endian layout -----------------------------------------
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint32_t offs_t;

union PAIR {
    struct { u8 l, h, h2, h3; } b;
    struct { u16 l, h; } w;
    struct { s8 l, h, h2, h3; } sb;
    struct { s16 l, h; } sw;
    u32 d;
    s32 sd;
};

union PAIR16 {
    struct { u8 l, h; } b;
    struct { s8 l, h; } sb;
    u16 w;
    s16 sw;
};

enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { INPUT_LINE_NMI = 32 };             // outside the Z180's own input lines (0-4), as in MAME

#define LOG(...) ((void)0)
#define logerror(...) ((void)0)

// ---- z180.h's enums ---------------------------------------------------------------------------------------------
enum {
    Z180_PC, Z180_SP, Z180_AF, Z180_BC, Z180_DE, Z180_HL, Z180_IX, Z180_IY, Z180_A, Z180_B, Z180_C, Z180_D, Z180_E,
    Z180_H, Z180_L, Z180_AF2, Z180_BC2, Z180_DE2, Z180_HL2, Z180_R, Z180_I, Z180_IM, Z180_IFF1, Z180_IFF2,
    Z180_HALT, Z180_DC0, Z180_DC1, Z180_DC2, Z180_DC3, Z180_CNTLA0, Z180_CNTLA1, Z180_CNTLB0, Z180_CNTLB1,
    Z180_STAT0, Z180_STAT1, Z180_TDR0, Z180_TDR1, Z180_RDR0, Z180_RDR1, Z180_CNTR, Z180_TRDR, Z180_TMDR0,
    Z180_TMDR1, Z180_RLDR0, Z180_RLDR1, Z180_TCR, Z180_ASEXT0, Z180_ASEXT1, Z180_FRC, Z180_ASTC0, Z180_ASTC1,
    Z180_CMR, Z180_CCR, Z180_SAR0, Z180_DAR0, Z180_BCR0, Z180_MAR1, Z180_IAR1, Z180_BCR1, Z180_DSTAT, Z180_DMODE,
    Z180_DCNTL, Z180_IL, Z180_ITC, Z180_RCR, Z180_CBR, Z180_BBR, Z180_CBAR, Z180_OMCR, Z180_IOCR,
    Z180_IOLINES
};

enum {
    Z180_TABLE_op, Z180_TABLE_cb, Z180_TABLE_ed, Z180_TABLE_xy, Z180_TABLE_xycb,
    Z180_TABLE_ex    // cycle counts for taken jr/jp/call and interrupt latency (rst opcodes)
};

enum {
    Z180_INPUT_LINE_IRQ0, Z180_INPUT_LINE_IRQ1, Z180_INPUT_LINE_IRQ2, Z180_INPUT_LINE_DREQ0, Z180_INPUT_LINE_DREQ1
};

// ---- the framework's stand-ins ----------------------------------------------------------------------------------
// An IM0 injected instruction (CONTRACT.md 4): while `on`, opcode and operand reads take the next acknowledge byte
// from the bus (byte n = 0, 1, ...) instead of memory.
struct z180_inject {
    int on, n;
    u8 bytes[8];                          // the acknowledge bytes read so far (the first eight)
};

inline u8 z180_ack_byte(const cpu_bus *bus, z180_inject *inj)
{
    int v = bus->irq_ack ? bus->irq_ack(bus->ctx, Z180_INT0, inj->n) : -1;
    u8 b = v < 0 ? 0xff : (u8)v;
    if (inj->n < 8)
        inj->bytes[inj->n] = b;
    inj->n++;
    return b;
}

struct z180_mem {                         // m_program, m_cprogram: memory at the physical (post-MMU) address
    const cpu_bus *bus;
    z180_inject *inj;                     // m_cprogram only (operand reads); null for m_program (data)
    u8 read_byte(offs_t a) const { return inj && inj->on ? z180_ack_byte(bus, inj) : bus->read(bus->ctx, a); }
    void write_byte(offs_t a, u8 v) const { bus->write(bus->ctx, a, v); }
};

struct z180_opcodes {                     // m_copcodes: opcode fetches
    const cpu_bus *bus;
    z180_inject *inj;
    u8 read_byte(offs_t a) const
    {
        if (inj->on)
            return z180_ack_byte(bus, inj);
        return bus->fetch ? bus->fetch(bus->ctx, a) : bus->read(bus->ctx, a);
    }
};

struct z180_io {                          // m_io: external I/O only (the internal registers never get here)
    const cpu_bus *bus;
    u8 read_byte(offs_t p) const { return bus->in(bus->ctx, (uint16_t)p); }
    void write_byte(offs_t p, u8 v) const { bus->out(bus->ctx, (uint16_t)p, v); }
};

struct z180_line_cb {                     // devcb_write_line: an unconnected output
    void operator()(int) const {}
};

struct device_z80daisy_interface {        // never instantiated: no daisy chain
    int z80daisy_irq_ack() { return 0xff; }
};

// the opcode functions each prefix defines (z180tbl.h's TABLE lists them)
#define Z180_OPS_ROW(p, h) \
    void p##_##h##0(); void p##_##h##1(); void p##_##h##2(); void p##_##h##3(); \
    void p##_##h##4(); void p##_##h##5(); void p##_##h##6(); void p##_##h##7(); \
    void p##_##h##8(); void p##_##h##9(); void p##_##h##a(); void p##_##h##b(); \
    void p##_##h##c(); void p##_##h##d(); void p##_##h##e(); void p##_##h##f();
#define Z180_DECLARE_OPS(p) \
    Z180_OPS_ROW(p, 0) Z180_OPS_ROW(p, 1) Z180_OPS_ROW(p, 2) Z180_OPS_ROW(p, 3) \
    Z180_OPS_ROW(p, 4) Z180_OPS_ROW(p, 5) Z180_OPS_ROW(p, 6) Z180_OPS_ROW(p, 7) \
    Z180_OPS_ROW(p, 8) Z180_OPS_ROW(p, 9) Z180_OPS_ROW(p, a) Z180_OPS_ROW(p, b) \
    Z180_OPS_ROW(p, c) Z180_OPS_ROW(p, d) Z180_OPS_ROW(p, e) Z180_OPS_ROW(p, f)

// ---- the class --------------------------------------------------------------------------------------------------
// Everything is public: the step driver (z180_mame.cpp) is part of the core and reads the registers directly.
class z180_device {
public:
    explicit z180_device(const cpu_bus *bus)
        : m_extended_io(false), m_asci_0(0, bus), m_asci_1(1, bus),
          m_cprogram{bus, &m_inject}, m_program{bus, nullptr}, m_copcodes{bus, &m_inject}, m_io{bus}, m_bus(bus)
    {
        m_inject.on = m_inject.n = 0;
        for (int i = 0; i < 8; i++)
            m_inject.bytes[i] = 0;
        m_inject_pending = 0;
        m_inject_pc0 = 0;
        m_inject_trapped = 0;
        m_asci[0] = &m_asci_0;
        m_asci[1] = &m_asci_1;
        m_csio = &m_csio_0;
    }
    virtual ~z180_device() {}

    void init_tables();                   // z180_mame_machine.cpp: device_start's flag-table part
    virtual void device_reset();
    void execute_set_input(int irqline, int state);
    bool get_tend0();
    bool get_tend1();

    // the framework's calls, answered
    bool daisy_chain_present() const { return false; }
    int daisy_update_irq_state() { return CLEAR_LINE; }
    void daisy_call_reti_device() {}
    device_z80daisy_interface *daisy_get_irq_device() { return nullptr; }
    int standard_irq_callback(int, offs_t)
    {
        int v = m_bus->irq_ack ? m_bus->irq_ack(m_bus->ctx, Z180_INT0, 0) : -1;
        return v < 0 ? 0xff : (v & 0xff);
    }
    void notify_clock_changed() {}        // CCR/CMR: our counts are in PHI T-states already

    // ---- our step driver's pieces (z180_mame.cpp): members, because MAME's macros name members ----
    int drv_accept(bool burst);           // phase A; returns the acceptance T-states
    bool drv_sleep_wake_request();        // an individually enabled request (ends SLEEP whatever IEF1 says)
    int drv_instruction();                // phase E: one instruction, or one HALT/SLP slot
    int drv_burst_chunk();                // phase E of a burst-DMA step
    bool drv_burst() const;               // is the next step a burst-DMA chunk?
    int drv_injected_instruction();       // phase E after an IM0 acceptance: the instruction from the acknowledge
    int drv_dispatch(uint8_t op);         // an instruction whose first byte is fetched: the prefixes, TRAP
    int drv_trap(bool ufo);               // an undefined opcode (z180_trap.hpp): TRAP to 0000h
    uint32_t m_inject_pc0;                // the PC the IM0 acceptance found
    int m_inject_trapped;                 // the injected instruction TRAPped
    z180_inject m_inject;                 // reads redirected to the acknowledge while on
    int m_inject_pending;                 // A accepted an IM0 request; E runs the injected instruction

    // ---- from z180.h (protected there) ----
    virtual uint8_t z180_internal_port_read(uint8_t port);
    virtual void z180_internal_port_write(uint8_t port, uint8_t data);

    // ---- from z180.h (private there) ----
    uint8_t z180_read_memory(offs_t addr) { return m_program.read_byte(addr); }
    void z180_write_memory(offs_t addr, uint8_t data) { m_program.write_byte(addr, data); }
    int memory_wait_states() const { return (m_dcntl & 0xc0) >> 6; }
    int io_wait_states() const { return (m_dcntl & 0x30) == 0 ? 0 : ((m_dcntl & 0x30) >> 4) + 1; }
    bool is_internal_io_address(uint16_t port) const { return ((port ^ m_iocr) & (m_extended_io ? 0xff80 : 0xffc0)) == 0; }

    const bool m_extended_io;
    PAIR      m_PREPC, m_PC, m_SP, m_AF, m_BC, m_DE, m_HL, m_IX, m_IY;
    PAIR      m_AF2, m_BC2, m_DE2, m_HL2;
    uint8_t   m_R, m_R2, m_IFF1, m_IFF2, m_HALT, m_IM, m_I;
    uint8_t   m_tmdr_latch;                     // flag latched TMDR0H, TMDR1H values
    uint8_t   m_read_tcr_tmdr[2];               // flag to indicate that TCR or TMDR was read
    uint32_t  m_iol;                            // I/O line status bits
    PAIR16    m_tmdr[2];                        // PRT data register ch 0-1
    PAIR16    m_rldr[2];                        // PRT reload register ch 0-1
    uint8_t   m_tcr;                            // PRT control register
    uint8_t   m_frc;                            // free running counter (also time base for ASCI, CSI/O & PRT)
    uint8_t   m_frc_prescale;                   // divide CPU clock by 10
    PAIR      m_dma_sar0;                       // DMA source address register ch 0
    PAIR      m_dma_dar0;                       // DMA destination address register ch 0
    PAIR16    m_dma_bcr[2];                     // DMA byte register ch 0-1
    PAIR      m_dma_mar1;                       // DMA memory address register ch 1
    PAIR      m_dma_iar1;                       // DMA I/O address register ch 1
    uint8_t   m_dstat;                          // DMA status register
    uint8_t   m_dmode;                          // DMA mode register
    uint8_t   m_dcntl;                          // DMA/WAIT control register
    uint8_t   m_il;                             // INT vector low register
    uint8_t   m_itc;                            // INT/TRAP control register
    uint8_t   m_rcr;                            // refresh control register
    uint8_t   m_mmu_cbr;                        // MMU common base register
    uint8_t   m_mmu_bbr;                        // MMU bank base register
    uint8_t   m_mmu_cbar;                       // MMU common/bank area register
    uint8_t   m_omcr;                           // operation mode control register
    uint8_t   m_iocr;                           // I/O control register
    offs_t    m_mmu[16];                        // MMU address translation
    uint8_t   m_tmdrh[2];                       // latched TMDR0H and TMDR1H values
    uint16_t  m_tmdr_value[2];                  // TMDR values used byt PRT0 and PRT1 as down counter
    uint8_t   m_nmi_state;                      // NMI line state
    uint8_t   m_nmi_pending;                    // NMI pending
    uint8_t   m_irq_state[3];                   // IRQ line states (INT0,INT1,INT2)
    uint8_t   m_int_pending[11 + 1];            // interrupt pending
    uint8_t   m_after_EI;                       // are we in the EI shadow?
    uint32_t  m_ea;
    uint8_t   m_rtemp;
    uint32_t  m_ioltemp;
    int m_icount;
    int m_extra_cycles;                         // extra cpu cycles
    uint8_t *m_cc[6];
    z180_line_cb m_tend0_cb, m_tend1_cb;

    // z180.cpp's file-level flag tables, as members
    uint8_t SZ[256];                            // zero and sign flags
    uint8_t SZ_BIT[256];                        // zero, sign and parity/overflow (=zero) flags for BIT opcode
    uint8_t SZP[256];                           // zero, sign and parity flags
    uint8_t SZHV_inc[256];                      // zero, sign, half carry and overflow flags INC r8
    uint8_t SZHV_dec[256];                      // zero, sign, half carry and overflow flags DEC r8
    std::unique_ptr<uint8_t[]> SZHVC_add;
    std::unique_ptr<uint8_t[]> SZHVC_sub;

    // the on-chip serial ports, owned; MAME's code reaches them through these pointers
    z180_asci m_asci_0, m_asci_1;
    z180_csio m_csio_0;
    z180_asci *m_asci[2];
    z180_csio *m_csio;

    z180_mem m_cprogram, m_program;
    z180_opcodes m_copcodes;
    z180_io m_io;
    const cpu_bus *m_bus;

    typedef void (z180_device::*opcode_func)();
    static const opcode_func s_z180ops[6][0x100];

    inline void z180_mmu();
    inline u8 RM(offs_t addr);
    inline u8 IN(u16 port);
    inline void OUT(u16 port, u8 value);
    inline void RM16(offs_t addr, PAIR *r);
    inline void WM16(offs_t addr, PAIR *r);
    inline uint8_t ROP();
    inline uint8_t ARG();
    inline uint32_t ARG16();
    inline uint8_t INC(uint8_t value);
    inline uint8_t DEC(uint8_t value);
    inline uint8_t RLC(uint8_t value);
    inline uint8_t RRC(uint8_t value);
    inline uint8_t RL(uint8_t value);
    inline uint8_t RR(uint8_t value);
    inline uint8_t SLA(uint8_t value);
    inline uint8_t SRA(uint8_t value);
    inline uint8_t SLL(uint8_t value);
    inline uint8_t SRL(uint8_t value);
    inline uint8_t RES(uint8_t bit, uint8_t value);
    inline uint8_t SET(uint8_t bit, uint8_t value);
    inline int exec_op(const uint8_t opcode);
    inline int exec_cb(const uint8_t opcode);
    inline int exec_dd(const uint8_t opcode);
    inline int exec_ed(const uint8_t opcode);
    inline int exec_fd(const uint8_t opcode);
    inline int exec_xycb(const uint8_t opcode);
    int take_interrupt(int irq);
    uint8_t z180_readcontrol(offs_t port);
    void z180_writecontrol(offs_t port, uint8_t data);
    int z180_dma0(int max_cycles);
    int z180_dma1();
    void z180_write_iolines(uint32_t data);
    void clock_timers();
    int check_interrupts();
    void handle_io_timers(int cycles);

    Z180_DECLARE_OPS(op)
    Z180_DECLARE_OPS(cb)
    Z180_DECLARE_OPS(dd)
    Z180_DECLARE_OPS(ed)
    Z180_DECLARE_OPS(fd)
    Z180_DECLARE_OPS(xycb)
    void illegal_1();
    void illegal_2();
};

class z8s180_device : public z180_device {
public:
    explicit z8s180_device(const cpu_bus *bus) : z180_device(bus), m_cmr(0), m_ccr(0) {}

    void device_reset() override;
    uint8_t z180_internal_port_read(uint8_t port) override;
    void z180_internal_port_write(uint8_t port, uint8_t data) override;

    uint8_t   m_cmr;                            // clock multiplier
    uint8_t   m_ccr;                            // chip control register
};

#endif
