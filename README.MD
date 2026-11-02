# Pikmin-PS3-Port

Pikmin for the PlayStation 3 (homebrew, PSL1GHT).

Pikmin-PS3-Port is a native PS3 port of [projectPiki's Pikmin decompilation](https://github.com/projectPiki/pikmin),
the source code of the GameCube game (2001) rebuilt so that it compiles to the original, byte for byte. The
game code is the decompilation's, almost untouched. What the game expected from the GameCube is rebuilt on
the PS3's own hardware:
- graphics, drawn by the RSX;
- the sound processor, running on an SPU;
- the disc, the memory card and the controller.

It runs on a PS3 with custom firmware (tested on Evilnat 4.93 with Cobra), at a steady 30 frames a second
like the original, with full sound. The whole game has been played through on a real console.

You need your own copy of the game: nothing from the disc is included in this repository or in the package.

---

## What you need

| File | Where it goes | Notes |
| --- | --- | --- |
| Pikmin disc image, North America (GPIE01), as `.iso`, `.gcm` or `.ciso` (any file name) | `/dev_hdd0/game/PIKM00001/USRDIR/` | Plain and CISO images both work. The port is made from revision 1, the most common disc; revision 0 hasn't been tried. |
| A USB drive (FAT32), only for the first start, if you have no FTP or file manager | top of the drive, or a `PIKMIN` folder on it | The game copies the image to the hard drive by itself. |

The game reads its files straight out of the disc image. Nothing is extracted or converted.

Without FTP or a file manager, put the image on a USB drive and start the game. On the first start it copies
the image to the hard drive, showing its progress on screen. This takes a minute or two and happens once;
after that the USB drive isn't needed. The European and Japanese discs don't work: the port is built from
the North American version.

Installing a newer package over an existing install keeps the disc image. Deleting the game from the XMB
deletes `USRDIR`, image included (your saves are kept, see below).

### Optional: the XMB icon and background

A package built from this repository comes with PSL1GHT's icon. On the first start the game replaces it with
the banner from your own disc, the picture the GameCube menu shows for it. It appears once the game has quit.

To use your own pictures instead, make them before building:

```bash
python3 tools/ps3/make_icon.py your_logo.png platform/ps3/pkg/ICON0.local.png        # 320 x 176 icon
python3 tools/ps3/make_background.py your_picture.jpg platform/ps3/pkg/PIC1.local.png # 1920 x 1080 background
```

`make_icon.py` removes a plain light background around the picture so the XMB shows through.
`make_background.py` scales and crops the picture to fill the screen. Both need Pillow (and `make_icon.py`
also needs numpy and SciPy). The package uses whichever of these files exist. They are ignored by git: keep
publishers' artwork out of the repository.

## Controls (DualShock 3)

| DualShock 3 | GameCube |
| --- | --- |
| Left stick | Control stick |
| Right stick | C-stick |
| Cross | A |
| Circle | B |
| Square | X |
| Triangle | Y |
| L1 | L (fully pressed) |
| L2 | L (analog) |
| R1 | R |
| R2 | Z |
| D-pad | D-pad |
| Start | Start |

Everything else works as in the GameCube game's manual. The sticks have the same dead zones and limits as on
the GameCube.

To quit, press the PS button and choose Quit Game. Quitting doesn't save: save at the end of a day, as on the
GameCube.

## Saves

Like on the GameCube, the game saves at the end of each day, to a memory card. The memory card is a file:
`/dev_hdd0/data/pikmin/MemoryCardA.USA.raw`, outside the game's folder, so it is kept if the game is deleted.
It's a GameCube memory card image (Memory Card 251) in the same format as Dolphin's `MemoryCardA.USA.raw`, so
a card can be copied between this port and Dolphin.

## Building

Requires the [ps3toolchain](https://github.com/ps3dev/ps3toolchain) and
[PSL1GHT](https://github.com/ps3dev/PSL1GHT) (built and tested with GCC 7.2 for the PPU and SPU), and Python 3.

```bash
export PS3DEV=/usr/local/ps3dev
export PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH

git clone https://github.com/MattyKen619/Pikmin-PS3-Port.git
cd Pikmin-PS3-Port
make -f Makefile.ps3 -j8 pkg
```

The output is `build/ps3/pikmin.pkg`, which installs from the XMB's Package Manager and appears as "Pikmin"
(title ID `PIKM00001`). The disc image isn't needed to build.

The game writes a log to `/dev_hdd0/game/PIKM00001/USRDIR/pikmin_log.txt`, and keeps the previous session's
as `pikmin_log_previous.txt`. If a USB drive with a `PIKMIN` folder is plugged in, a copy goes there too,
which is the easiest way to get at it. Crashes and freezes are always reported in the log. An empty file
named `diagnostics.txt` in that `PIKMIN` folder turns on the testing extras: a profiler, frame recordings
(also on Select), and more frequent status reports.

The original GameCube build of the decompilation still works and still matches. See
[README.decomp.md](README.decomp.md).

## How it works (short version)

- `src/`, `include/`: the decompiled game code, almost untouched. The PS3 build compiles the same files for a
  64-bit CPU. A handful of `TARGET_PS3` changes fix places where the original read memory it shouldn't have:
  harmless on a GameCube, but fatal on a PS3.
- `platform/ps3/sdk/`: the parts of Nintendo's GameCube SDK that the game calls, rewritten for the PS3:
  - threads and timers;
  - the disc, read from the disc image;
  - the memory card, controllers, video timing and audio.
- `platform/ps3/gpu/` and `platform/ps3/rsx/`: the GameCube's graphics commands are interpreted and drawn by
  the RSX. Shader programs are generated for the GameCube's texture and colour setups as the game uses them.
  The picture is drawn at the GameCube's resolution and shown in 4:3, as large as the TV allows.
- `platform/ps3/spu/`: the GameCube's sound processor, reimplemented on an SPU.
- `platform/ps3/backend/`: the PS3 system layer (PSL1GHT and lv2): files, threads, the start-up screen and
  the log, finding and copying the disc image, and the XMB icon.
- `tools/ps3/`: build helpers and tests that run on a computer: graphics and sound checks, the disc image
  reader, and the icon.

## Credits

- [Pikmin decompilation](https://github.com/projectPiki/pikmin): projectPiki and its contributors, built with
  [decomp-toolkit](https://github.com/encounter/decomp-toolkit) and [objdiff](https://github.com/encounter/objdiff).
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and [ps3toolchain](https://github.com/ps3dev/ps3toolchain): the ps3dev
  team. The fragment program assembler in `platform/ps3/rsx/fpasm/` comes from PSL1GHT's cgcomp, with headers from
  the nouveau and Mesa projects (MIT, see `platform/ps3/rsx/fpasm/`).
- 8x8 console font: Daniel Hepper, after Marcel Sondaar and IBM's VGA fonts (public domain).
- [Doom64-PS3](https://github.com/stingziz0u/doom64-ps3) by stingziz0u: this README's layout, and the way it
  handles the player's own game data.
- Testing on real hardware: MattyKen619.
- Pikmin: Nintendo EAD, published by Nintendo.

This project is not affiliated with or endorsed by Nintendo. Pikmin is a trademark of Nintendo.

## License

CC0 1.0 (see `LICENSE.MD`), like the decompilation, except where a file says otherwise (the MIT-licensed
fragment program assembler). No game data is included or distributed.

## AI disclosure

This port's code was written with Claude (Anthropic), working with MattyKen619, who tested every build on a
PS3 and guided the work. The game code itself is the decompilation's.
