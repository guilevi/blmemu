The Braille Lite Millennium 40 on macOS, Windows and Linux. It runs Freedom Scientific's own firmware on an emulated
Z180, and its DoubleTalk speech runs RC Systems' own firmware on an emulated M16C (librc8650).

**You need two firmwares, which don't come with it:** `BLM40ENU.BNS`, the Millennium 40 update (revision 4.00), and
RC Studio, RC Systems' free software, whose `Bin` folder holds the RC8650's firmware. The program asks for both on its
first start and checks them. See the README.

| Download | |
|---|---|
| `blmemu-…-macos.zip` | macOS 11 or later, Apple silicon and Intel. Open `Braille Lite Millennium.command`; the first time, Control-click (VoiceOver: VO-Shift-M) and choose Open. |
| `blmemu-…-windows-x64.zip` | Windows 10 or later. Run `blmemu.exe`. |
| `blmemu-…-linux-x86_64.tar.gz` | A terminal; needs ALSA's `libasound2`. |

What works: the unit from a cold start, its speech at your sound device's own rate, the 8-dot keyboard and space bar,
the 40-cell display (shown as Unicode braille for screen readers), its 2 MB of RAM and 12 MB of flash kept between
runs, its clock, and the Braille Display Mode switch.

Not yet: the advance bars, Whiz Wheels and cursor routing keys; the serial and printer ports; CompactFlash.

This is a first release. The Windows window program and the Linux build haven't been tried on real machines by the
author yet; reports are welcome in the issues.
