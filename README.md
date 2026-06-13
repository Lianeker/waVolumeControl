# wkVolumeControl

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/28d6b09e-64fa-47f4-add8-c1a05f83e0d8" />

WormKit module for Worms Armageddon 3.8+ that lets you adjust, independently
and **while playing**:

- **Music** — the volume of the game's music track
- **Effects** — the volume of sound effects and voices
- **Master** — the application's overall volume (the same control as the
  Windows volume mixer)

## Installation

1. Copy `wkVolumeControl.dll` to your Worms Armageddon folder (next to
   `WA.exe`).
2. Enable module loading: in W:A 3.8, `Options` → `Advanced` → tick
   **"Load WormKit modules"**. (Alternative: launch the game through
   `WormKit.exe`.)

## Usage

- Press **Scroll Lock** during a match (or in the menus) to show or hide
  the volume panel.
- Drag the sliders with the mouse, or use the **mouse wheel** over a row
  for fine adjustment. The panel never steals focus from the game.
- Values are saved to `wkVolumeControl.ini` and restored on startup.

## Configuration (`wkVolumeControl.ini`)

```ini
[Volumes]
Music=100
Effects=100
Master=100

[Settings]
ToggleKey=145          ; virtual-key code in decimal (145 = Scroll Lock)
MusicBufferMinKB=0     ; optional size-based override (0 = disabled)
MusicLockThreshold=4   ; refills after which a buffer is considered music
Debug=0                ; 1 = write wkVolumeControl.log
Language=auto          ; panel language: en, es or auto (system locale)
```

Useful key codes: 145 = Scroll Lock, 19 = Pause, 35 = End, 36 = Home,
45 = Insert. Full list: search "virtual-key codes" on MSDN.

## How it works / troubleshooting

The module intercepts `DirectSoundCreate` (the same entry point the
SoundCardSelect module uses) and classifies every sound buffer the game
creates by its behaviour: a sound effect is written to its buffer once when
loaded, while music is a *streaming* buffer the game refills continuously.
Once a buffer exceeds `MusicLockThreshold` refills it is reclassified as
music. Each category's volume is applied as attenuation on top of whatever
volume the game requests for each sound.

- **The music slider drags some effect along (or vice versa):** set
  `Debug=1`, play for a bit, and check `wkVolumeControl.log` — you'll see
  every buffer created and which ones get promoted to MUSIC. Raise
  `MusicLockThreshold` if a long effect gets promoted by mistake.
- **The music/effects sliders do nothing at all:** check the log; if it
  says `WARNING: no DirectSoundCreate import found`, your game version
  creates sound through a different path — open an issue with the log.
- The **Master** slider always works (it uses the Windows audio session,
  independent of the game).
- Old game versions that play music from the CD (CD audio) are not covered
  by the music slider.

## Building

Requires Visual Studio Build Tools (x86 compiler). Run `build.bat`.
The DLL must be **32-bit** (WA.exe is a 32-bit executable). No external
dependencies.
