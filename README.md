# s1pro

A CHIRP-style desktop editor for the **Hytera S1 Pro** (HYT-S35) handheld DMR radio.
It programs the radio over Bluetooth LE from a computer, so you do not need the phone app.

Unofficial. Not affiliated with or endorsed by Hytera.

## What it does

- Reads and writes digital channels, analog channels, contacts and zones.
- Spreadsheet-style grids: copy, cut and paste cells or whole rows (tab-separated, so it
  works to and from a spreadsheet), duplicate, delete and reorder rows.
- Zone editor: pick and order the channels of each zone.
- Imports the PMR446 and LPD433 band plans as analog channels.
- Saves the whole configuration to a `.s1pro` file (plain JSON).
- Before writing, shows the exact list of changes; after writing, reads the radio back and
  reports anything that did not stick.

## Status and limits

Developed and tested against one radio: S1 Pro, 400-470 MHz, firmware A1.2.04.005.

- Most columns show the radio's stored numbers. Frequencies (MHz) and tones (Hz) are converted.
- Adding and deleting whole zones is untested.
- Radio-wide settings (power, keys, tones, language) are not editable yet.
- Developed and tested on Linux. The macOS and Windows builds are untested on real hardware.

This tool writes to your radio's configuration. The commands were learned by observing the
official app, not from documentation, so use it at your own risk and keep a saved copy of a
known-good configuration.

## Download

Every commit is built by GitHub Actions for Linux (x86_64 and arm64 / Raspberry Pi), macOS and
Windows. Open the latest successful run under the repository's **Actions** tab and download the
artifact for your system.

- **macOS**: unzip and open `s1pro.app`. It is not signed by an Apple developer, so the first
  time right-click it and choose Open (or run `xattr -dr com.apple.quarantine s1pro.app`).
  macOS will ask to allow Bluetooth.
- **Windows**: unzip the folder and run `s1pro.exe`.

The macOS and Windows builds use a different Bluetooth layer from the Linux one and have not
been tested on real hardware yet; reports are welcome.

## Build

Linux, on a PC (x86_64) or a Raspberry Pi with 64-bit Raspberry Pi OS (arm64). The same script
builds for whichever machine you run it on:

    ./build.sh

It tells you if anything is missing. On Debian, Ubuntu and Raspberry Pi OS the dependencies are:

    sudo apt install g++ cmake ninja-build qt6-base-dev

Running it also needs BlueZ, the standard Linux Bluetooth service (package `bluez`).

macOS and Windows: install Qt 6 with the Qt Bluetooth (Connectivity) module, then

    cmake -S . -B build
    cmake --build build --config Release

There are two Bluetooth backends, chosen with `-DS1PRO_BLUETOOTH=`: `bluez` (default on Linux,
talks to BlueZ directly) and `qt` (default elsewhere, uses Qt Bluetooth; also works on Linux
with `qt6-connectivity-dev`).

## Use

    build/s1pro                 open the editor
    build/s1pro --read          open the editor and read the radio straight away
    build/s1pro file.s1pro      open a saved configuration
    build/s1pro info            print the radio's identity and table sizes
    build/s1pro dump            print every table, tab-separated

On macOS the program inside the bundle is `s1pro.app/Contents/MacOS/s1pro`.

Turn the radio on with Bluetooth enabled and make sure no phone is connected to it. The first
read scans for the radio and remembers its address.

Only one program can talk to the radio at a time; a second copy of s1pro will wait its turn.

## How it works

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the Bluetooth protocol.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
