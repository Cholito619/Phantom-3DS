<div align="center">

# 👻 Phantom

**A clean speedrun timer for the Nintendo 3DS.**
Splits, personal bests, gold splits, touch controls, and your own game list, all on the console.

![Platform](https://img.shields.io/badge/platform-Nintendo%203DS-3cb043)
![Requires](https://img.shields.io/badge/requires-Luma3DS-8e7cc3)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

</div>

---

## Features

- **Your own games.** Create a game on the console with the built-in keyboard: name, category, and your split names.
- **Live comparison.** See how far ahead or behind your personal best you are on every split, color-coded.
- **Gold splits.** Your best-ever segments are tracked and highlighted, with a running sum of best.
- **Big, readable timer** on the top screen with a progress strip. The split list and controls live on the touch screen.
- **Touch and button controls.** Tap SPLIT, RESET, or Save & Exit, or use the physical buttons.
- **Reset confirmation** so a stray tap can't end a good run. You can turn it off in Settings.
- **Settings.** Toggle the reset prompt, music, and volume. Reset progress for one game or all games, or delete a game.
- **Background music.** A short built-in tune, or drop in your own `music.wav`.
- **Saves automatically** to the SD card. Your times survive restarts.

## Requirements

| Need | Why |
|---|---|
| A Nintendo 3DS family console with **custom firmware (Luma3DS)** | Needed to install and run homebrew |
| **FBI** | Installs the `.cia` so Phantom appears on the HOME menu |
| *(Optional)* **DSP firmware dump** | Only needed for music. Rosalina menu: *Miscellaneous options → Dump DSP firmware* |

Developed and tested on a New Nintendo 3DS XL.

## Install

### Option 1: Scan the QR code (easiest)

On your 3DS, open **FBI → Remote Install → Scan QR Code** and point the camera at this code. FBI downloads and installs Phantom.

<div align="center">
  <img src="PhantomFiles/qr.jpg" alt="QR code to install Phantom with FBI" width="220">
</div>

### Option 2: Download the file

1. Download the `.cia` from the [latest release](../../releases/latest).
2. Copy it to your SD card and install it with **FBI → SD**.
3. Launch **Phantom** from the HOME menu.

### Option 3: Homebrew Launcher

Download the `.3dsx` from the [latest release](../../releases/latest), copy it to `/3ds/` on your SD card, and launch it from the Homebrew Launcher.

## Using Phantom

1. On the welcome screen, tap **+ NEW GAME** and enter a game name, a category, and your splits. Press **Done** after the last split.
2. Tap your game to open it.
3. Tap **START** (or press **A**) to begin, then **SPLIT** for each segment.
4. Finish the last split to see your result. A new personal best is saved automatically.

### Controls

| Action | Touch | Button |
|---|---|---|
| Start / Split | SPLIT | A |
| Reset | RESET | SELECT |
| Settings | Gear icon | X |
| Save & Exit | Save & Exit | START |
| Menu navigation | Tap | D-pad, A |
| Popup yes / no | YES / NO | A / B |

### Your data

Everything lives in `sdmc:/3ds/speedtimer/`:

```
settings.txt                 your settings
music.wav                    optional: your own music (16-bit PCM WAV, mono or stereo)
games/<game>/game.txt        name and category
games/<game>/splits.txt      one split name per line
games/<game>/data.txt        saved PB and gold splits
```

To rename or reorder splits, edit `splits.txt` for that game. Changing the number of splits clears that game's saved PB and golds.

## Building from source

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS development group. For the installable CIA you also need `makerom` and `bannertool` on your `PATH`.

```bash
make                       # builds the .3dsx
bash meta/build_cia.sh     # builds the .cia (edit the name at the top of the script first)
```

The Makefile must link the graphics libraries: `LIBS := -lcitro2d -lcitro3d -lctru -lm`.

```
source/main.c      the app
meta/              icon, HOME menu banner, banner sound, CIA spec, build script
assets/            README images
```

## Troubleshooting

- **No music:** dump the DSP firmware (see Requirements). Settings shows the music status.
- **Installed app won't launch:** rebuild the CIA with the `meta/cia.rsf` from this repo and install it over the old one with FBI.
- **A game won't open:** check that its `splits.txt` exists and has at least one line.

## Disclaimer

Phantom is an unofficial, fan-made homebrew tool. It is not affiliated with or endorsed by Nintendo. It does not include any game content.

## License

Released under the [MIT License](LICENSE).
