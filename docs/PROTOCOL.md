# Hytera S1 Pro Bluetooth programming protocol

Worked out from Bluetooth packet captures of the official "Hytera Radio" Android app talking to
an S1 Pro (HYT-S35, firmware A1.2.04.005). Nothing here comes from Hytera documentation, and
field meanings are inferred, so treat it as a description of observed behaviour.

Multi-byte integers are big-endian in the link and application headers and little-endian inside
parameter data. The traffic is not encrypted and needs no pairing.

## Bluetooth LE

| Service | Characteristic | Use |
|---|---|---|
| `0783b03e-8535-b5a0-7140-a304d2495cb7` | `...5cba` | write without response: computer to radio |
| | `...5cb8` | notify: radio to computer |
| | `...5cb9` | flow control (unused here) |
| `02f00000-0000-0000-0000-00000000fe00` | | firmware update; not used |

The advertisement carries manufacturer ID 4098 and the radio's alias as its name. A frame larger
than the ATT payload is simply split over several writes or notifications and must be reassembled
using the length field.

## Link layer

    5E 6E | 01 | op | src | dst | pn(2) | 00 00 00 01 01 00 00 00 | len(2) | cksum(2) | payload

- `op`: `FE` connect, `FD` accept, `00` data, `10` acknowledge.
- `src`/`dst`: `20` is the computer, `10` the radio.
- `pn`: packet number, counted separately by each side; an acknowledgement repeats it.
- `len`: whole frame, header included (the header is 20 bytes).
- `cksum`: 16-bit internet checksum (one's complement sum) of the frame with this field zeroed.

Session: send `FE`, expect `FD` (the first `FE` is often lost; resend). Every data frame is
acknowledged by the receiver with op `10`. Unacknowledged frames are resent with the same `pn`.

## Application layer

    0A 0B | cmd(2) | module(2) | seq(4) | len(2) | data

A reply repeats `seq` and uses `cmd | 0x1000`.

| cmd | module | Meaning |
|---|---|---|
| `0003` | `0002` | Identity query. data = item(2). Reply: status(2) item(2) len(2) UTF-16BE text. Items: 1 firmware, 11 serial, 2 codeplug version. |
| `0101` | `0006` | Parameter access (below). |
| `2F00` | | Subscribe to change notifications (the app sends `31 01 00 06`). |
| `3101` | | Unsolicited change notification from the radio. |
| `01F0`, `41F8` | | Whole-file codeplug read used by the app's sync; not used by this tool. |

The app opens every session with `0101`/`0006` data `00 02 00 01 00 00 00 01 00`.

## Parameter access

Request data: `02 | item count | items`. Reply data: an 8-byte header, then reply items.

Reply header: byte 0 is `02` when more frames follow and `00` on the last; byte 5 is the frame's
position in the reply; byte 7 is the item count of that frame.

Every setting has a numeric parameter ID (32-bit). A field is `id(4) len(2) value`. Integers are
little-endian; text is UTF-16LE (zero-padded to 32 bytes when read, exact length when written).

| Item | Meaning | Reply item |
|---|---|---|
| `03 03 n ids` | read scalar parameters | `03 04 n fields` |
| `03 13 row n ids` | read parameters of one table row | `03 14 row n fields` |
| `06 03 01 id` | number of rows in the table that owns `id` | `06 06 01 id count(2)` |
| `03 61 zone first 07 06 FFFF` | read a zone's channel list | `03 64 zone entry 01 07 06 len(=4) channel(2) type(2)` per entry |
| `02 14 row n fields` | write parameters of one table row | status only |
| `00 04 n fields` | append a row to the table that owns the fields | status only |
| `01 07 id 01 row 01` | delete a row | status only |
| `00 14 zone 02 (1323, channel)(1324, type)` | append a channel to a zone | status only |
| `01 17 zone 1323 01 entry 01` | remove the zone entry at that position | status only |

`row`, `zone`, `entry`, `n` and `count` are 16-bit. Rows, zones and entries count from 0; channel
numbers inside zones count from 1; type is 0 for digital and 1 for analog.

Several read items can go in one request. A reply too long for one frame continues in the next:

- a list frame ends with a marker item that has the normal start but zeros where the count would
  be (`03 64 zone entry 00 00`, `03 14 row 00 00`), naming where the next frame resumes;
- a table row can also be split, appearing as two `03 14` items for the same row, each with some
  of its fields.

A write reply starts with `00 00` on success. The radio accepts zone entries without checking for
duplicates.

## Tables

| Table | Key parameter | Parameters |
|---|---|---|
| Digital channels | 917 alias | 941 slot, 931 colour code, 924 RX Hz, 925 TX Hz, 937 contact ID, 938 contact type, and others |
| Analog channels | 690 alias | 697 RX Hz, 698 TX Hz, 699/701 RX/TX tone in 0.1 Hz, 700/702 tone type, and others |
| Contacts | 972 alias | 973 DMR ID, 974 type (1 group, 0 private) |
| Zones | 1325 alias | 1326 prefix, 1327 type; channel list via `03 61` |

The full ID lists are in `src/tables.cpp`.
