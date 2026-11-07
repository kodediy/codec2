# Codec 2 for ESP-IDF

[Codec 2](https://www.rowetel.com/codec2.html) is David Rowe's open, patent-free speech codec for very low bit
rates, 3200 down to 700 bit/s. This repository is the build of it that Meshtastic's firmware uses on the ESP32,
packaged as an **ESP-IDF component** and still usable as an Arduino / PlatformIO library. Kode keeps it for
MeshKode, the mesh walkie-talkie app of the Kode Dot (ESP32-P4).

## Where it comes from

| When | Who | What |
|---|---|---|
| 2010-2015 | David Rowe and the Codec 2 contributors | `codec2-dev` ([drowe67/codec2-dev](https://github.com/drowe67/codec2-dev)). The library here is its commit `7e1cef218` of 24 Sep 2015. |
| 2020 | Deulis Antonio Pelegrin Jaime, [deulis/ESP32_Codec2](https://github.com/deulis/ESP32_Codec2) | The library cut down for the ESP32 under Arduino: variable-length arrays made fixed (`LPC_ORD`), unused code commented out, a LoRa walkie example and audio helpers (`ButterworthFilter`, `SampleFilter`, `FastAudioFIFO`). Added the GPL-3.0 `LICENSE`. |
| 2023 | Thomas Göttgens, [meshtastic/ESP32_Codec2](https://github.com/meshtastic/ESP32_Codec2) | `src/` layout and `library.properties` for PlatformIO, warnings silenced. Meshtastic's AudioModule builds this. |
| 2026 | Kode, [kodediy/codec2](https://github.com/kodediy/codec2) | ESP-IDF component, const tables, the 700/700B filter fix, a host check: [Changes in this fork](#changes-in-this-fork). |

Until this fork's 700/700B fix none of those steps changed what the codec computes: `tools/host_test.sh --against`
a checkout of `codec2-dev` `7e1cef218` gives identical encoded bits and decoded samples in all eight modes for the
tree before that fix (macOS arm64, Apple clang 21).

## Using it

### ESP-IDF 6.1 or later

```yaml
# main/idf_component.yml
dependencies:
  esp32_codec2:
    git: https://github.com/kodediy/codec2.git
    version: v1.1.0
```

```cmake
# main/CMakeLists.txt
idf_component_register(SRCS "main.c" REQUIRES esp32_codec2)
```

```c
#include "codec2.h"

struct CODEC2 * c = codec2_create(CODEC2_MODE_1600);
int nsam  = codec2_samples_per_frame(c);           /* 320 */
int nbyte = (codec2_bits_per_frame(c) + 7) / 8;    /* 8 */
codec2_encode(c, bits, pcm);                       /* nsam samples at 8 kHz in, nbyte bytes out */
codec2_decode(c, pcm, bits);
codec2_destroy(c);
```

ESP-IDF names a managed component after its directory, which is the dependency's key (`esp32_codec2` above), so
`REQUIRES` names that key. Only `codec2.h` and `codec2_fifo.h` are public (`include/`); `src/` has headers named
`os.h`, `defines.h`, `comp.h` that stay private. The library is built with `-O2` whatever the project's level, and
with `-w`. `codec2_create()` `malloc`s its state; the tables are `const` and stay in flash.

### Arduino and PlatformIO

As before: `library.properties` and `src/`. The ESP-IDF files (`CMakeLists.txt`, `idf_component.yml`, `include/`)
are not used there.

### Plain CMake

Outside ESP-IDF, `CMakeLists.txt` builds a static `codec2` library.

## Modes, and the ones MeshKode uses

| Mode | `CODEC2_MODE_` | bit/s | samples per frame (8 kHz) | bytes per frame |
|---|---|---|---|---|
| 0 | `3200` | 3200 | 160 (20 ms) | 8 |
| 1 | `2400` | 2400 | 160 (20 ms) | 6 |
| 2 | `1600` | 1600 | 320 (40 ms) | 8 |
| 3 | `1400` | 1400 | 320 (40 ms) | 7 |
| 4 | `1300` | 1300 | 320 (40 ms) | 7 (52 bits) |
| 5 | `1200` | 1200 | 320 (40 ms) | 6 |
| 6 | `700`  | 700  | 320 (40 ms) | 4 (28 bits) |
| 7 | `700B` | 700  | 320 (40 ms) | 4 (28 bits) |

MeshKode can use **all eight**: for the radio settings in use it picks the highest bit rate whose packets fit the
air time, and it plays whatever mode a received packet names. Its decoders turn the LPC post filter on as
Meshtastic's AudioModule does, `codec2_set_lpc_post_filter(c, 1, 0, 0.8f, 0.2f)`. The calls it makes are
`codec2_create`, `codec2_destroy`, `codec2_samples_per_frame`, `codec2_bits_per_frame`,
`codec2_set_lpc_post_filter`, `codec2_encode` and `codec2_decode`.

## Compatibility: the rule for every change

MeshKode and Meshtastic's AudioModule talk to each other over 2.4 GHz LoRa: a packet is a 4-byte header (the magic
`0xC0 0xDE 0xC2` and the mode number) followed by whole Codec 2 frames. So **every change here must keep upstream
Codec 2's bitstream**: mode numbers, frame sizes, bit packing (`pack.c`, Gray coding), quantisers and codebooks stay
as they are, and a frame encoded here must decode anywhere else, and the other way round.

Two promises, kept apart:

- **Bitstream compatibility** (the format) is not negotiable.
- **Bit-exactness** (the same input giving the same bits and the same samples) is what an optimisation proves on
  the test signal before it goes in: `tools/host_test.sh --against <the tree before it>` must say `identical` in
  every row. Codec 2 is float code calling libm, so exact output can only be compared on one host, with one
  compiler and one set of flags; it is not expected to match between macOS, Linux and the ESP32-P4. A change that
  cannot be bit-exact (another FFT, a reordered sum) says so in its commit, shows how much changes, and updates
  `tools/host_test.golden` in that same commit.

One trap: Codec 2 has a single random generator for the whole program (`codec2_rand()` in `sine.c`), shared by every
decoder instance and not thread-safe, so a decoder's output depends on what other decoders did before it.

## The host check

```sh
tools/host_test.sh                       # build for this computer, encode and decode every mode, check the hashes
tools/host_test.sh --update              # record this host's hashes in tools/host_test.golden
tools/host_test.sh --against ../old/src  # A/B: the same harness on another tree's sources, mode by mode
```

It compiles `src/*.c` (but `dump.c`) with `cc -O2 -fno-fast-math -ffp-contract=off` together with
`tools/host_test.c`. That harness makes a 6 s synthetic signal with integer arithmetic only (pitch-gliding voiced
segments through formant resonators, noise bursts, mixed excitation, silence: the same bytes on every host), and
each mode is encoded and decoded in a process of its own, because of `codec2_rand()`. The check fails on a hash
that differs from this host's line in `tools/host_test.golden`, on a decode that is near silence, on two encodes
that differ, and, with `--against`, on any byte or sample that differs. `CC` and `CFLAGS_EXTRA` pass through;
`KEEP=1` keeps the work directory with its `.bit` and `.raw` files.

## Changes in this fork

- ESP-IDF component: `CMakeLists.txt`, `idf_component.yml`, the public headers in `include/`.
- `bpf[]` and `bpfb[]`, the 700/700B band-pass filters, are `const`: 808 bytes leave RAM for flash without a
  linker fragment. Bit-exact.
- The 700 and 700B encoders filtered with 102 taps of a 101-tap table, reading one float past its end, so their
  output depended on the link layout (upstream never fixed it, and removed both modes in 2019). They now use the 101
  taps. The bitstream format and the decoder are untouched; on the test signal 20 of mode 700's 600 bytes change.
- `tools/host_test.sh`, `tools/host_test.c`, `tools/host_test.golden`.

## Licence and authors

- `LICENSE` is the GNU General Public License, version 3, under which deulis/ESP32_Codec2 published the repository.
- The Codec 2 sources keep their notices, unchanged: Copyright (C) 2009-2015 David Rowe (Perens LLC for `pack.c`),
  GNU Lesser General Public License version 2.1.
- `kiss_fft.c`, `kiss_fft.h`, `_kiss_fft_guts.h`: Copyright (c) 2003-2010 Mark Borgerding, BSD 3-clause licence
  (its text is in `kiss_fft.c`).
- `ButterworthFilter`, `SampleFilter`, `FastAudioFIFO` and the Arduino example: Deulis Antonio Pelegrin Jaime.
- The files Kode added (`tools/host_test.*`) are GPL-3.0-or-later.

`idf_component.yml` declares `LGPL-2.1-only AND BSD-3-Clause`: that is what the component compiles (the Codec 2 sources
and kiss_fft). The GPL-3.0 of `LICENSE` covers the Arduino helpers and the host check, which the component does not
build.
