// z180_asci.hpp -- the Z180's on-chip serial ports for the MAME-derived core: ASCI channels 0 and 1, and a CSI/O stub.
//
// Our own code (MIT), not MAME's: MAME's ASCI runs on the device framework's timers, bit by bit.  This one is
// byte-level and clocked in T-states by the step driver (z180_mame.cpp calls catch_up at phase D, CONTRACT.md 1).
// Its register methods carry the names MAME's z180.cpp calls (cntla_r, stat_w, ...), so the extracted internal-
// register code (z180_mame_machine.cpp) uses it unchanged.
//
// What it models, and from where:
//  - Divisors from the chip's own registers (chip; Zilog Z8018x UM, ASCI): bit time = prescale (10 or 30, PS) x
//    2^SS x the sampling divide (16 or 64, DR; 1 with ASEXT X1).  With ASEXT BRGM the prescale x 2^SS term is
//    ASTC + 2, as MAME computes it (the Braille Lite firmware never writes ASEXT or ASTC).  SS = 7 (external CKA
//    clock) stops the channel: the Braille Lite has no CKA source.
//  - Byte level, as the legacy core (legacy, kept as model): when a channel's receiver is enabled and idle it asks
//    the bus for a byte once per bit time (serial_rx); a byte then takes one frame (start + data + parity + stop
//    bits) before it reaches the receive FIFO.  A transmitted byte leaves the chip (serial_tx) one frame after it
//    enters the shift register; TDR moves into the shift register when that is empty.
//  - The interrupt request is a level that follows the status bits : RIE with RDRF, OVRN, PE or
//    FE (and, channel 0, /DCD0 high); TIE with TDRE.  So clear_interrupt() does nothing.
//  - The receive FIFO holds 4 bytes (chip, Z8S180).  The legacy core's ring held 3.
//  - /DCD0 comes from bus->serial_pin(Z180_PIN_DCD0), sampled at each catch-up; /CTS is taken as asserted.
#ifndef SSI263_Z180_ASCI_HPP
#define SSI263_Z180_ASCI_HPP

#include <cstdint>
#include "cpu.h"

class z180_asci {
public:
    z180_asci(int index, const cpu_bus *bus) : m_index(index), m_bus(bus) { reset(); }

    void reset();
    // Bring the baud clock up to T-state `now`: bits received and sent, bytes delivered.  Phase D.
    void catch_up(uint64_t now);
    // IOSTOP: the baud clock does not run; time up to `now` passes without a tick.
    void hold(uint64_t now) { if (m_next_tick <= now) m_next_tick += ((now - m_next_tick) / m_sample_div + 1) * m_sample_div; }

    // the registers, by MAME's names
    uint8_t cntla_r() const { return m_cntla; }
    uint8_t cntlb_r() const { return (uint8_t)(m_cntlb & ~0x20); }   // bit 5 reads /CTS: low, asserted (as MAME)
    uint8_t stat_r() const;
    uint8_t tdr_r() const { return m_tdr; }
    uint8_t rdr_r();
    uint8_t asext_r() const { return m_asext; }
    uint8_t astcl_r() const { return (uint8_t)m_astc; }
    uint8_t astch_r() const { return (uint8_t)(m_astc >> 8); }
    void cntla_w(uint8_t data);
    void cntlb_w(uint8_t data);
    void stat_w(uint8_t data);
    void tdr_w(uint8_t data);
    void rdr_w(uint8_t data);
    void asext_w(uint8_t data);
    void astcl_w(uint8_t data);
    void astch_w(uint8_t data);

    int check_interrupt() const;          // the request level, now
    void clear_interrupt() {}             // a level: acceptance does not clear it

private:
    void update_rate();
    void receive(uint8_t data);
    void bit_time();                      // one bit time: receiver, then transmitter (the legacy order)

    const int m_index;
    const cpu_bus *m_bus;

    uint8_t m_cntla, m_cntlb, m_stat, m_tdr, m_rdr, m_asext;
    uint16_t m_astc;
    int m_dcd;                            // /DCD0 asserted (carrier present); channel 0 only

    uint8_t m_fifo[4], m_fifo_err[4];
    int m_fifo_rd, m_fifo_n;

    int m_frame_bits;                     // start + data + parity + stop
    uint32_t m_sample_div;                // T-states per baud tick: 16, 64 or 1
    uint32_t m_brg;                       // baud ticks per bit; 0 = stopped (external clock)
    uint64_t m_next_tick;                 // the T-state of the next baud tick
    uint32_t m_brg_count;                 // ticks left in the current bit time

    int m_rx_bits;                        // bit times left for the byte being received (0 = idle)
    uint8_t m_rx_data;
    int m_tx_bits;                        // bit times left for the byte in the shift register (0 = empty)
    uint8_t m_tx_data;
};

// The clocked serial port, with an external clock only (cpu.h z180_csio_clock: the device on the far end clocks each
// byte; the Blazie units' clock controller does).  EF is set when a byte has moved and cleared by reading or writing
// TRDR; the interrupt request is EF and EIE, a level.  The Z180's own clock (SS < 111) is not modelled.
class z180_csio {
public:
    void reset() { m_cntr = 0x07; m_trdr = 0; }
    uint8_t cntr_r() const { return m_cntr; }
    void cntr_w(uint8_t data) { m_cntr = (uint8_t)((m_cntr & 0x80) | (data & 0x7f)); }
    uint8_t trdr_r() { m_cntr &= 0x7f; return m_trdr; }
    void trdr_w(uint8_t data) { m_cntr &= 0x7f; m_trdr = data; }
    int check_interrupt() const { return (m_cntr & 0xc0) == 0xc0; }
    void clear_interrupt() {}
    // the external clock's 8 bits: TRDR out (TE), `in` into TRDR (RE); returns the bits that were armed
    int clock(uint8_t in, uint8_t *sent)
    {
        int armed = m_cntr & 0x30;
        if (!armed)
            return 0;
        if ((armed & 0x10) && sent)
            *sent = m_trdr;
        if (armed & 0x20)
            m_trdr = in;
        m_cntr = (uint8_t)((m_cntr & ~0x30) | 0x80);
        return armed;
    }

private:
    uint8_t m_cntr = 0x07, m_trdr = 0;
};

#endif
