// z180_asci.cpp -- the Z180's ASCI for the MAME-derived core.  What it models, and why: z180_asci.hpp.
#include "z180_asci.hpp"

namespace {
enum : uint8_t {                          // STAT
    STAT_TIE = 0x01, STAT_TDRE = 0x02, STAT_DCD0 = 0x04, STAT_CTS1E = 0x04, STAT_RIE = 0x08,
    STAT_FE = 0x10, STAT_PE = 0x20, STAT_OVRN = 0x40, STAT_RDRF = 0x80,
    STAT_ERRORS = STAT_OVRN | STAT_PE | STAT_FE,
};
enum : uint8_t {                          // CNTLA
    CNTLA_MOD0 = 0x01, CNTLA_MOD1 = 0x02, CNTLA_MOD2 = 0x04, CNTLA_EFR = 0x08, CNTLA_TE = 0x20, CNTLA_RE = 0x40,
};
enum : uint8_t {                          // CNTLB
    CNTLB_SS = 0x07, CNTLB_DR = 0x08, CNTLB_PS = 0x20, CNTLB_MP = 0x40,
};
enum : uint8_t {                          // ASEXT (Z8S180)
    ASEXT_BRGM = 0x08, ASEXT_X1 = 0x10, ASEXT_DCD0_DISABLE = 0x40,
};
}

void z180_asci::reset()
{
    m_cntla = m_index ? 0x00 : 0x10;      // channel 0: RTS0 high
    m_cntlb = 0x07;                       // SS = 7: no clock until the firmware chooses a rate
    m_stat = STAT_TDRE;
    m_tdr = m_rdr = 0;
    m_asext = 0;
    m_astc = 0;
    m_fifo_rd = m_fifo_n = 0;
    for (int i = 0; i < 4; i++)
        m_fifo[i] = m_fifo_err[i] = 0;
    m_rx_bits = m_tx_bits = 0;
    m_rx_data = m_tx_data = 0;
    m_next_tick = 0;
    m_brg = 0;
    m_brg_count = 0;
    m_dcd = m_index == 0 && m_bus->serial_pin ? (m_bus->serial_pin(m_bus->ctx, Z180_PIN_DCD0) ? 1 : 0) : 1;
    update_rate();
}

void z180_asci::update_rate()
{
    uint32_t brg;
    m_frame_bits = 1 + ((m_cntla & CNTLA_MOD2) ? 8 : 7) + ((m_cntla & CNTLA_MOD0) ? 2 : 1)
                   + ((m_cntlb & CNTLB_MP) ? 1 : ((m_cntla & CNTLA_MOD1) ? 1 : 0));
    m_sample_div = (m_asext & ASEXT_X1) ? 1 : ((m_cntlb & CNTLB_DR) ? 64 : 16);
    if ((m_cntlb & CNTLB_SS) == CNTLB_SS)
        brg = 0;                          // external clock (CKA): none on this board
    else if (m_asext & ASEXT_BRGM)
        brg = (uint32_t)m_astc + 2;
    else
        brg = (1u << (m_cntlb & CNTLB_SS)) * ((m_cntlb & CNTLB_PS) ? 30 : 10);
    if (brg != m_brg) {                   // a new rate starts a new bit time; other writes leave the phase alone
        m_brg = brg;
        m_brg_count = brg;
    }
}

void z180_asci::catch_up(uint64_t now)
{
    uint64_t ticks;
    if (m_index == 0 && m_bus->serial_pin)
        m_dcd = m_bus->serial_pin(m_bus->ctx, Z180_PIN_DCD0) ? 1 : 0;
    if (m_next_tick > now)
        return;
    ticks = (now - m_next_tick) / m_sample_div + 1;
    m_next_tick += ticks * m_sample_div;
    if (!m_brg)
        return;
    while (ticks >= m_brg_count) {
        ticks -= m_brg_count;
        m_brg_count = m_brg;
        bit_time();
    }
    m_brg_count -= (uint32_t)ticks;
}

void z180_asci::bit_time()
{
    // receiver: the byte on the line arrives one frame after it starts (legacy order: receiver first).  The next
    // start bit may follow the last stop bit at once, so back-to-back bytes take one frame each (8N1: 10 bit
    // times; the legacy core took 12).
    if ((m_cntla & CNTLA_RE) && (m_index != 0 || m_dcd || (m_asext & ASEXT_DCD0_DISABLE))) {
        if (m_rx_bits > 0 && --m_rx_bits == 0)
            receive(m_rx_data);
        if (m_rx_bits == 0 && m_bus->serial_rx) {
            int c = m_bus->serial_rx(m_bus->ctx, m_index);
            if (c >= 0) {
                m_rx_data = (uint8_t)c;
                m_rx_bits = m_frame_bits;
            }
        }
    }
    // (RE cleared mid-byte freezes the byte rather than dropping it, as the legacy core does: the host has already
    // handed it over.)

    // transmitter
    if (m_cntla & CNTLA_TE) {
        if (m_tx_bits > 0 && --m_tx_bits == 0 && m_bus->serial_tx)
            m_bus->serial_tx(m_bus->ctx, m_index, m_tx_data);
        if (m_tx_bits == 0 && !(m_stat & STAT_TDRE)) {
            m_tx_data = m_tdr;            // TDR into the empty shift register
            m_tx_bits = m_frame_bits;
            m_stat |= STAT_TDRE;
        }
    }
}

void z180_asci::receive(uint8_t data)
{
    if (m_fifo_n == 4) {                  // overrun: the byte is lost, the last one in the FIFO carries the flag
        m_fifo_err[(m_fifo_rd + 3) & 3] |= STAT_OVRN;
    } else {
        int wr = (m_fifo_rd + m_fifo_n) & 3;
        m_fifo[wr] = data;
        m_fifo_err[wr] = 0;
        m_fifo_n++;
    }
    m_stat |= STAT_RDRF;
}

uint8_t z180_asci::stat_r() const
{
    if (m_index == 0)                     // DCD0 reads the /DCD0 pin: 1 = high = no carrier
        return (uint8_t)((m_stat & ~STAT_DCD0) | (m_dcd ? 0 : STAT_DCD0));
    return m_stat;
}

uint8_t z180_asci::rdr_r()
{
    if (m_fifo_n) {
        m_rdr = m_fifo[m_fifo_rd];
        m_stat = (uint8_t)((m_stat & ~STAT_ERRORS) | m_fifo_err[m_fifo_rd]);
        m_fifo_rd = (m_fifo_rd + 1) & 3;
        if (--m_fifo_n == 0)
            m_stat &= ~STAT_RDRF;
    }
    return m_rdr;
}

void z180_asci::cntla_w(uint8_t data)
{
    m_cntla = data;
    if (!(data & CNTLA_EFR))              // EFR = 0 resets the error flags: UNVERIFIED (recalled from Zilog's manual,
        m_stat &= ~STAT_ERRORS;           // not checked; MAME clears them on EFR = 1).  Never exercised yet.
    update_rate();
}

void z180_asci::cntlb_w(uint8_t data)
{
    m_cntlb = data;
    update_rate();
}

void z180_asci::stat_w(uint8_t data)
{
    uint8_t mask = m_index ? (STAT_RIE | STAT_CTS1E | STAT_TIE) : (STAT_RIE | STAT_TIE);
    m_stat = (uint8_t)((m_stat & ~mask) | (data & mask));
}

void z180_asci::tdr_w(uint8_t data)
{
    m_tdr = data;
    m_stat &= ~STAT_TDRE;
    if ((m_cntla & CNTLA_TE) && m_tx_bits == 0) {
        m_tx_data = m_tdr;                // straight into the empty shift register: TDR is free again
        m_tx_bits = m_frame_bits;
        m_stat |= STAT_TDRE;
    }
}

void z180_asci::rdr_w(uint8_t data)
{
    if (!(m_stat & STAT_RDRF))            // loopback into the receiver (Zilog UM)
        receive(data);
}

void z180_asci::asext_w(uint8_t data)
{
    m_asext = data;
    update_rate();
}

void z180_asci::astcl_w(uint8_t data)
{
    m_astc = (uint16_t)((m_astc & 0xff00) | data);
    update_rate();
}

void z180_asci::astch_w(uint8_t data)
{
    m_astc = (uint16_t)((m_astc & 0x00ff) | (data << 8));
    update_rate();
}

int z180_asci::check_interrupt() const
{
    int rx = (m_stat & STAT_RIE)
             && ((m_stat & (STAT_RDRF | STAT_ERRORS))
                 || (m_index == 0 && !m_dcd && !(m_asext & ASEXT_DCD0_DISABLE)));
    int tx = (m_stat & STAT_TIE) && (m_stat & STAT_TDRE);
    return rx || tx;
}
