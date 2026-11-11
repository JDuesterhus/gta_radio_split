# RadioSplit

32-bit ASI for GTA III, Vice City, and San Andreas. While a car radio station is playing, `RadioSplit.exe` plays that station and Windows lists it as its own volume-mixer entry. Guns, engines, speech, ambience, cutscenes, and the on-foot police scanner stay in the game mix.

This repository is the plugin source. It does not contain GTA executables, radio tracks, or other game files.

## Install

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (the usual `vorbisFile.dll` in the game folder).
2. Build with `build.bat`, or use a `RadioSplit.asi` / `RadioSplit.exe` you already built.
3. Copy `RadioSplit.asi`, `RadioSplit.exe`, and `RadioSplit.ini` into the game folder, next to the game executable. The ASI can also live in `scripts`. The exe has to sit next to `gta_sa.exe`, `gta-sa.exe`, `gta3.exe`, or `gta-vc.exe`.
4. Start the game once. `RadioSplit.log` lists playback devices.
5. An empty `OutputDevice` opens the Windows default playback device. The log names that device after it opens. Set `OutputDevice` to force one from the list.

`MuteInGame=0` also leaves the station in the game mix. `UserTracksInGame=1` leaves the San Andreas custom-file player on the game speakers. `WctrInGame=1` does the same for West Coast Talk Radio. User tracks default to off. WCTR stays in the game unless you set `WctrInGame=0`.

## What to listen for

- In a car, the volume mixer shows `RadioSplit.exe`, and the station comes from that entry.
- A gunshot, a voice line, and a cutscene stay on the game device.
- Radio off, and on foot, the side device stays silent.
- The menu music slider and pause affect the side output.
- Removing the ASI restores the stock mix.

The station can sit a few tens of milliseconds off the engine noise. Retune static can remain on the game speakers.

## Supported games

One `RadioSplit.asi` covers all three games. It looks at the executable name.

San Andreas is DirectSound. The plugin searches the running executable for the radio functions, so 1.0 US (compact and hoodlum), 1.01, 2.0, 3.0, and the Steam `gta-sa.exe` are covered. Packed executables are scanned after they unpack. `RadioSplit.log` prints the hardware, manager, and vehicle addresses it found.

GTA III and Vice City play radio through Miles (`mss32.dll`). The ASI decodes the station before `RadioSplit.exe` plays it. III station files are IMA ADPCM. Vice City opens station names such as `AUDIO\ESPANT.mp3`; the file on disk is `AUDIO\ESPANT.ADF`, an MP3 with each byte XOR-ed by `0x22`. Builds that are not 1.0, 1.1, or Steam still get the Miles hook. `RadioSplit.log` records each stream as `radio` or `left in game`.

## If III or Vice City stays in the game mix

`Miles radio tap installed` means the hooks attached. `Miles stats` is written every two seconds: `loads` is how often Miles submitted a buffer, `matched` is how often that buffer belonged to a radio stream, and `copied` is decoded 16-bit PCM bytes written for `RadioSplit.exe`. The game radio stays audible when a buffer does not decode. `stream_sample_handle=00000000` is expected on the Steam `mss32.dll`. A later line `Miles sample pointer is at stream+N` is the offset that linked them.

## Build

`build.bat` compiles the gate tests, `RadioSplit.asi`, and `RadioSplit.exe` with the x86 Visual C++ toolset. The 32-bit compiler comes from Visual Studio Build Tools.
