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
- Linux only for now (uses BlueZ). macOS and Windows need a different Bluetooth backend.

This tool writes to your radio's configuration. The commands were learned by observing the
official app, not from documentation, so use it at your own risk and keep a saved copy of a
known-good configuration.

## Build

Linux, including 64-bit Raspberry Pi OS:

    sudo apt install g++ cmake ninja-build qt6-base-dev
    ./build.sh

## Use

    build/s1pro                 open the editor
    build/s1pro --read          open the editor and read the radio straight away
    build/s1pro file.s1pro      open a saved configuration
    build/s1pro info            print the radio's identity and table sizes
    build/s1pro dump            print every table, tab-separated

Turn the radio on with Bluetooth enabled and make sure no phone is connected to it. The first
read scans for the radio and remembers its address.

Only one program can talk to the radio at a time; a second copy of s1pro will wait its turn.

## How it works

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the Bluetooth protocol.
