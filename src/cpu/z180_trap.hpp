// z180_trap.hpp -- which prefixed opcodes the Z180 defines; every other one TRAPs.  Our own (MIT), from Zilog's
// Z8018x Family MPU User Manual (UM005001-ZMP0400), the op code maps, printed pages 247-251:
//
//   Table 48 (1st op code, XX)   all 256 defined (CB, DD, ED, FD are prefixes).
//   Table 49 (CB XX)             all defined except 30-37 (the Z80's undocumented SLL).
//   Table 48, note 3 (DD/FD XX)  defined only before an instruction with HL or (HL) as an operand, which then uses
//                                IX/IY or (IX/IY+d); JP (HL) becomes JP (IX/IY); EX DE,HL is explicitly illegal.
//                                So H and L as plain registers (the Z80's IXH/IXL/IYH/IYL forms) are undefined.
//                                DD/FD CB leads to the DDCB/FDCB map.
//   DDCB/FDCB d XX               Table 49's (HL) forms only (XX & 7 == 6), SLL (HL) = 36 excluded.
//   Table 50 (ED XX)             the cells listed in ed_defined below.
//
// MAME's vendored tables implement several of these as the Z80 does (IX halves, SLL, ED duplicates such as
// ED 54 NEG, ED 70 IN (C)); this table, not MAME's, decides what traps.  z180_mame.cpp's drv_instruction applies
// it before MAME's dispatch (the vendored files stay unchanged).
#ifndef SSI263_Z180_TRAP_HPP
#define SSI263_Z180_TRAP_HPP

#include <cstdint>

namespace z180_trap {

inline bool cb_defined(uint8_t op) { return op < 0x30 || op > 0x37; }

inline bool xycb_defined(uint8_t op) { return (op & 7) == 6 && op != 0x36; }

inline bool xy_defined(uint8_t op)        // the byte after DD or FD
{
    switch (op) {
    case 0x09: case 0x19: case 0x29: case 0x39:                        // ADD IX,rr
    case 0x21: case 0x22: case 0x23: case 0x2a: case 0x2b:             // LD IX,mn; LD (mn),IX; INC/DEC IX; LD IX,(mn)
    case 0x34: case 0x35: case 0x36:                                   // INC/DEC (IX+d); LD (IX+d),n
    case 0x46: case 0x4e: case 0x56: case 0x5e: case 0x66: case 0x6e: case 0x7e:   // LD r,(IX+d)
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x77:   // LD (IX+d),r
    case 0x86: case 0x8e: case 0x96: case 0x9e: case 0xa6: case 0xae: case 0xb6: case 0xbe:   // ALU A,(IX+d)
    case 0xcb:                                                         // the DDCB/FDCB map
    case 0xe1: case 0xe3: case 0xe5: case 0xe9: case 0xf9:             // POP, EX (SP), PUSH, JP (IX); LD SP,IX
        return true;
    default:
        return false;
    }
}

inline bool ed_defined(uint8_t op)        // Table 50, cell by cell
{
    switch (op) {
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x38:   // IN0 g,(m)
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x39:   // OUT0 (m),g
    case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x3c:   // TST g
    case 0x34:                                                                     // TST (HL)
    case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x78:   // IN g,(C)
    case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x79:   // OUT (C),g
    case 0x42: case 0x52: case 0x62: case 0x72:                                    // SBC HL,ww
    case 0x4a: case 0x5a: case 0x6a: case 0x7a:                                    // ADC HL,ww
    case 0x43: case 0x53: case 0x63: case 0x73:                                    // LD (mn),ww
    case 0x4b: case 0x5b: case 0x6b: case 0x7b:                                    // LD ww,(mn)
    case 0x4c: case 0x5c: case 0x6c: case 0x7c:                                    // MLT ww
    case 0x44:                                                                     // NEG
    case 0x45: case 0x4d:                                                          // RETN, RETI
    case 0x46: case 0x56: case 0x5e:                                               // IM 0, 1, 2
    case 0x47: case 0x4f: case 0x57: case 0x5f:                                    // LD I,A; LD R,A; LD A,I; LD A,R
    case 0x64: case 0x74: case 0x76:                                               // TST m; TSTIO m; SLP
    case 0x67: case 0x6f:                                                          // RRD, RLD
    case 0x83: case 0x8b: case 0x93: case 0x9b:                                    // OTIM, OTDM, OTIMR, OTDMR
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa8: case 0xa9: case 0xaa: case 0xab:   // LDI..OUTD
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb8: case 0xb9: case 0xba: case 0xbb:   // LDIR..OTDR
        return true;
    default:
        return false;
    }
}

}  // namespace z180_trap

#endif
