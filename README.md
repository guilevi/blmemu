# Braille Lite Millennium emulator

The Freedom Scientific Braille Lite Millennium 40, a braille notetaker from 2001, as a program for macOS, Windows and
Linux. It runs the unit's own firmware on an emulated Z180, and its DoubleTalk speech runs the RC8650's own firmware on
an emulated M16C (librc8650). Nothing is scripted or recorded. The unit's own program does the talking, shows its own
braille on its 40-cell display, and keeps its files in its own RAM and flash.

## What you need

Neither firmware comes with the emulator. You bring your own copies, once:

- **The Millennium's firmware: `BLM40ENU.BNS`**, the Millennium 40 update file (revision 4.00, February 2003) from
  Freedom Scientific's update disk.
- **The DoubleTalk's firmware**, from **RC Studio**, RC Systems' free development software for their speech chips.
  Its `Bin` folder holds the RC8650's firmware. You can give the emulator the RC Studio folder, its `Bin` folder, or
  the file `m$4a95b2.bin` in it. An RC8650 `.srec` image made with librc8650's `tools/rcstudio2srec.py` works too. On
  Windows, an installed RC Studio is found by itself.

The emulator checks each file and copies it into its data folder (below). After that you don't need the originals.

## The first start

**macOS.** Unzip, then open `Braille Lite Millennium.command` (it opens Terminal). The program isn't notarized, so the
first time macOS asks: open it from its context menu (Control-click, or VoiceOver's VO-Shift-M) and choose Open. Or run
`./blmemu` from Terminal.

**Windows.** Unzip and run `blmemu.exe`. Windows SmartScreen may warn about an unknown program: More info, then Run
anyway.

**Linux.** Unpack and run `./blmemu` in a terminal. It needs ALSA's library (`libasound2`, on every desktop).

It asks for the two firmwares (a terminal: type or drag in the path, then Enter; Windows: a file dialog), then sets up
a new unit. That takes a few seconds: the hard reset a unit had at the factory, every question answered yes. Then the
unit starts and says "Millennium forty ready".

## Keys

The Millennium's keyboard is 8-dot: 7 3 2 1 4 5 6 8, left to right, and the space bar. On the PC keyboard it is the
home row in the same order:

| Key | |
|---|---|
| A S D F J K L ; | dots 7 3 2 1 4 5 6 8 |
| space bar | the space bar |
| Enter, Backspace | dot 8, dot 7 |
| arrows | the dot-1 (up), dot-4 (down), dot-3 (left) and dot-6 (right) chords |

**A chord** is keys pressed together, as on the unit. On Windows the keys go to the unit as they go down and come up,
just as its own keyboard does. In a terminal, which only reports keys pressed, the keys typed within 80 ms of each
other are one chord.

The unit's commands are the unit's: its manual is on Freedom Scientific's disk (`BLMMAN.txt`). For example, with the
space bar: dot 1 is the top of the file, dot 4 the end, and dots 1-5-6 (z-chord) leave a menu or file.

## The program's own commands

**Terminal (macOS, Linux):** F11 or Ctrl+O opens the menu. Type its number and Enter; Enter alone goes back to the
unit.

| | |
|---|---|
| 1 | Restart the unit: switched off and on, its files kept |
| 2 | Hard reset: restart with I-chord held. The unit asks its questions; answer them with y or n |
| 3 | Back to the factory state: erases this unit's files (type yes) |
| 4 | The Braille Display Mode switch |
| 5 | Show the braille display, as Unicode braille, each time it changes |
| 6 | Show what the unit says, a line at a time |
| 7, 8 | Bring in the Millennium's or the DoubleTalk's firmware again |
| 9 | The keys |
| a | About: the version, the data folder, the sound's rate |
| 0 | Exit: the unit is switched off and its memory saved |

Shortcuts: Ctrl+B and Ctrl+T toggle 5 and 6, Ctrl+D the switch, Ctrl+R restarts, Ctrl+Q exits. Ctrl+K holds the next
chord down until you press Ctrl+K again. A chord held through a restart is held while the unit starts, as on the real
one.

**Windows:** the menu bar (Alt).
- **Unit:** Restart (Ctrl+R), Restart holding I-chord, Back to the factory state, Bring in either firmware, Exit.
- **Settings:** Braille display in the window (Alt+Shift+B), the Braille Display Mode switch.
- **Help:** Keys, About.

The braille display field shows the unit's 40 cells as Unicode braille, which your screen reader puts on your own
braille display. It keeps the keyboard, so the unit's keys work as before. Turn off your screen reader's braille
translation for it.

## The switches

The Millennium has two switches on its sides: power (right) and **Braille Display Mode** (left). In Braille Display Mode
the unit is a braille display for a PC's screen reader over its serial port, which the emulator doesn't connect yet
(below). Leave it off for the notetaker.

## Sound

The DoubleTalk is played at your output device's own sample rate (48 kHz, 44.1 kHz, or the lower rates a Bluetooth
headset switches to when its microphone comes on). When the device or its rate changes, the emulator follows. The
voice, its rate, pitch and volume are set on the unit, with its own commands.

## What the unit keeps

A real unit keeps its 2 MB of RAM, its 12 MB of flash and its clock while switched off. The emulator does the same.
When you exit, it switches the unit off (the firmware saves what it keeps) and writes its memory to the data folder.
It also saves once a minute while running. The data folder:

| System | Folder |
|---|---|
| macOS | `~/Library/Application Support/blmemu` |
| Windows | `%APPDATA%\blmemu` |
| Linux | `~/.config/blmemu` |

It holds `firmware/` (the two you brought in), `blm40.state` (the unit's memory) and `blmemu.ini` (settings; the
terminal's keys can be changed there, section `[keys]`: `dot1 = f`, ..., `dot8 = semicolon`). `--home DIR` uses
another folder.

The clock starts from your computer's date and time and runs on from there. Set it with the unit's own commands.

## Not yet

- The advance bars, the Whiz Wheels and the cursor routing keys.
- The serial port (a PC: file transfer, Braille Display Mode), the printer port and CompactFlash.
- The Millennium 20, and firmware updates run on the unit.

## Building

You need CMake, a C/C++ compiler, and `git submodule update --init` (librc8650 is a submodule). Then:

    tools/prepare_m32c.sh            # once: fetches GDB 14.2, takes its m32c simulator, applies librc8650's patch
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

- **Windows** from macOS or Linux with mingw-w64: `cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake`.
- **Linux** needs `libasound2-dev`.
- **A release:** `tools/package.sh macos|linux|windows BUILD_DIR`.
- **CI:** `.github/workflows/build.yml` builds all three.

`build/blm_run` runs the unit headless: keys on a schedule, what it says, its display, a WAV, traces of the hardware.
Run it without arguments for its options. `src/blm_board.h` describes the Millennium's hardware as the firmware uses
it, and how each part was found.

## Thanks

A huge thank you to **[ssi263-speech](https://github.com/tgeczy/ssi263-speech)** and everyone behind it. This emulator
wouldn't exist without it. ssi263-speech brought the Blazie units back to life: the Braille Lite 2000, the Braille 'n
Speak and the Type 'n Speak, running their own firmware. The Millennium is their direct descendant, and almost
everything here stands on that work:

- **The CPU.** The Z180, MAME's core behind ssi263-speech's step driver and its carefully worked-out timing contract,
  runs here unchanged.
- **The board.** Its shape and its way of working come from the Braille Lite 2000's: the firmware's banked calls, the
  power switch, the flash and the clock found by running the firmware and reading what it does. Even the braille
  display's dot wiring is the same, and decoding it was a matter of checking the Millennium against what ssi263-speech
  had already measured.
- **The way it's used.** The keys on the home row, chords from a terminal, the F11 menu, the braille display field for
  screen readers, the unit switched off and saved as a person would, and a new unit set up as it left the factory. All
  of it follows the Blazie emulator, so anyone who knows that one will feel at home here.
- **The approach.** Evidence first, the firmware as the authority, and the person listening as the final judge.

Thank you for doing that work so carefully, and for making it open.

## Licence

GPL-3.0-or-later (`LICENSE`), because librc8650 and GDB's simulator are GPL-3. The Z180 core is MAME's (BSD-3-Clause),
behind a driver from ssi263-speech's Blazie emulator (MIT). `licenses/NOTICE.md` lists every part. The firmware is not
part of the program and keeps its own terms.
