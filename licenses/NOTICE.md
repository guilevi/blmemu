# What is in this program, and under which licence

The program as a whole is under the GNU General Public License, version 3 or later (`LICENSE`), because it contains
GPL-3 code (librc8650 and GDB's simulator). Its parts:

| Part | Where it comes from | Licence |
|---|---|---|
| The Millennium's board, the DoubleTalk host, the shells | this project | GPL-3.0-or-later |
| The Z180's step driver and its contract (`src/cpu/`, except `mame_z180/`); the board's structure | ssi263-speech's Blazie emulator | MIT (`ssi263-speech-MIT.txt`) |
| The Z180 core (`src/cpu/mame_z180/`) | MAME | BSD-3-Clause (`MAME-Z180-core-BSD-3-Clause.txt`) |
| librc8650: the RC8650 on an emulated M16C | librc8650 | GPL-3.0-or-later |
| The M16C simulator | GDB 14.2's `sim/m32c`, patched by librc8650 | GPL-3.0-or-later |
| rcdict (inside librc8650) | librc8650 | BSD-3-Clause |
| Remimu, the regular expressions inside rcdict | Remimu | CC0 |

**The firmware is not part of the program.** The Braille Lite Millennium's firmware is Freedom Scientific's, and the
RC8650's is RC Systems'. Neither comes with this program. You bring your own copies, and they keep their own terms.
