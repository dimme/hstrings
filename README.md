# hstrings

`hstrings` finds continuous sequences of printable characters hidden inside a
binary file or on standard input. Plain `strings` only sees text that is stored
verbatim; `hstrings` first undoes the transforms that malware and packers
commonly use to hide text, so strings that were XORed, rotated, bit-shifted,
added to, encoded, or stored as wide characters show up too. It can also rank
everything it finds by how likely it is to be human-readable, so the strings
worth looking at come first.

## Transforms

Every label names the *decoding* operation that was applied to the input
before the strings were found.

| Label             | Undoes                                                               |
| ----------------- | -------------------------------------------------------------------- |
| `XOR-0xKK`        | every byte XORed with the single-byte key `KK`                       |
| `XORKEY-HEX`      | a repeating multi-byte XOR key                                       |
| `XORINC-0xKK`     | rolling XOR: byte *i* XORed with `KK + i`                            |
| `XORDEC-0xKK`     | rolling XOR: byte *i* XORed with `KK - i`                            |
| `XORCHAIN`        | each byte XORed with the previous input byte                         |
| `ADD-0xKK`        | `KK` subtracted from every byte (`ADD-(256-KK)` undoes an addition)   |
| `ROL-N` / `ROR-N` | each byte rotated on its own by N bits                               |
| `SHL-N`           | the whole bit stream shifted by N bits                               |
| `BITREV`          | the bit order of every byte reversed                                 |
| `ROL-N+XOR-0xKK`  | a rotation combined with an XOR key (`--rotxor`, opt-in)             |
| `BASE64` / `HEX`  | runs of base64 or hex text in the raw input, decoded                 |

A few notes on what is and is not generated, and why:

- `ROL`/`ROR` rotate each byte on its own, which is what you want when the
  data was obfuscated byte by byte. `SHL` shifts the entire bit stream, which
  is what you want when the text sits at an offset that is not a multiple of
  eight bits: the bits of one character are then split across two input bytes
  and a per-byte rotation recovers only a garbled version. Shifting the
  stream right by N yields the same characters as shifting it left by 8-N, so
  only the left direction is generated.
- `ROL-4` and `ROR-4` are the same rotation, so the default set lists it
  once. `--rol` and `--ror` ask for a direction explicitly and get all seven.
- XOR-then-rotate produces the same set of passes as rotate-then-XOR with a
  different key, so `--rotxor` generates one order only (7 × 256 passes).
- `--xor-guess` derives repeating keys of 1 to N bytes from byte
  frequencies. Binaries are full of zero bytes, so the most frequent byte at
  each key position is usually the key byte itself. Keys that are just a
  shorter key repeated are skipped.
- `BASE64` and `HEX` only report a run whose decoded bytes are entirely text.
  Identifiers in the binary's own strings are valid base64 and hex too, and
  that rule keeps them from producing garbage.

Each transformed buffer is searched for 8-bit strings and, by default, for
16-bit little-endian (Windows wide) strings as well; those carry a `/16LE`
suffix on the label. `-e` selects other widths and byte orders.

With no transform option, every transform except `--rotxor` is run: about
1050 passes, each searched for 8-bit and 16-bit strings. A pass that finds
nothing prints no label.

## Ranking

`--rank` collects every string from every pass, drops exact duplicates, and
prints them best first with a 0–100 readability score:

```
$ hstrings --rank --top=5 -t d sample.bin
100  XOR-0x5A          5008 http://evil.example.com/gate.php
100  XOR-0x00          5015 kernel32.dll
 96  XOR-0x00          5028 This is a readable sentence hidden in the file.
 93  XOR-0x21/16LE     5137 Software\Microsoft\Windows\CurrentVersion\Run
 83  XOR-0x20          5080 SENTENCE  [x2]
```

The score is a heuristic. Its main signal is how likely each pair of adjacent
letters is in English, from a table of bigram costs derived from about 40 MB
of prose and source comments. That is blended with how much of the run is
letters, spaces and ordinary punctuation, whether the vowel ratio is in the
range prose has, whether it is made of words, and how long it is. Runs that
are mostly symbols, one repeated character, alternating characters, mostly
non-letters, that flip case mid-word, or that string punctuation together are
marked down. Runs containing a URL, path, DLL name, registry hive or shell
name are marked up. A run is scored as a whole and also piecewise, split on
clusters of symbols, and the best piece wins, so a real string still ranks
high when the bytes next to it happened to come out printable too.

`--top=N` limits the list (default 100; `--top=0` for everything). Ranking
holds only the current best N in memory, so it is safe on large inputs.

## Threads

Passes are independent, so by default `hstrings` runs one worker thread per
CPU and hands passes out to them. Each thread writes a pass's output into its
own buffer and the main thread prints those buffers in pass order, so the
output is byte-for-byte the same whatever the thread count, in both the
grouped and the ranked mode. A thread may run only a few passes ahead of the
printer, which bounds the memory held in finished-but-unprinted output.

`-j N` / `--threads=N` sets the count. On a 1 MB input the default sweep
takes about 7.5 s single-threaded and about 2.2 s on four cores.

## Building

```bash
make          # build
make test     # build and run the test suite
```

`CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS` and `PREFIX` can be overridden:

```bash
make CC=clang CFLAGS="-O1 -g -fsanitize=address,undefined"
```

## Usage

```
hstrings [OPTION]... [FILE]
```

With no `FILE`, or when `FILE` is `-`, input is read from standard input.

Output options:

| Option                | Meaning                                                        |
| --------------------- | -------------------------------------------------------------- |
| `-n`, `--min-len=LEN` | print runs of at least LEN characters (default 4)              |
| `-t`, `--radix=RADIX` | print the offset of each run; RADIX is `d`, `x` or `o`         |
| `-e`, `--encoding=L`  | encodings to search, comma separated: `s` 8-bit, `l` 16-bit LE, `b` 16-bit BE, `L` 32-bit LE, `B` 32-bit BE (default `s,l`) |
| `--color[=WHEN]`      | colourise labels; WHEN is `auto` (default), `always`, `never`  |
| `--rank`              | print the most readable strings first, duplicates removed       |
| `--top=N`             | with `--rank`, print only the best N (default 100, 0 for all)   |
| `-j`, `--threads=N`   | run N passes in parallel (default: one per CPU)                |
| `-h`, `--help`        | display help and exit                                          |
| `-V`, `--version`     | output version information and exit                            |

Transform options. Give none to run all of them except `--rotxor`; give any
to run only those:

| Option            | Passes                                                             |
| ----------------- | ------------------------------------------------------------------ |
| `--xor[=KEY]`     | every single-byte key, or just KEY: a byte (`0x4F`, `79`) or a hex string (`0xDEADBEEF`) for a repeating multi-byte key |
| `--xor-guess[=N]` | repeating keys up to N bytes long guessed from byte frequencies (default 8) |
| `--xor-roll`      | `XORINC-0x00..FF` and `XORDEC-0x00..FF`                            |
| `--xor-chain`     | `XORCHAIN`                                                         |
| `--add`           | `ADD-0x01..FF`                                                     |
| `--rol`, `--ror`  | `ROL-1..7`, `ROR-1..7`                                             |
| `--shl`           | `SHL-1..7`                                                         |
| `--bitrev`        | `BITREV`                                                           |
| `--rotxor`        | `ROL-1..7` combined with every XOR key (1792 passes)               |
| `--base64`, `--hex` | decode runs of base64 / hex in the raw input                     |

Colour is written only when standard output is a terminal, so piping into
`grep` or `less` gives clean text.

## Examples

See what is worth looking at, without reading through every pass:

```bash
./hstrings --rank sample.bin
```

Scan a file with every transform, grouped by pass:

```bash
./hstrings sample.bin
```

```
XOR-0x00:
(sequence 1)
(sequence 2)
XOR-0x00/16LE:
(sequence 1)
XOR-0x4F:
(sequence 1)
```

The full sweep produces a lot of output, so it is usually worth narrowing it
down. Longer runs only, with hex offsets, from the XOR family alone:

```bash
./hstrings --xor -n 8 -t x sample.bin
```

Try a known multi-byte key, or let the tool guess one:

```bash
./hstrings --xor=0xDEADBEEF sample.bin
./hstrings --xor-guess=16 sample.bin
```

Search for 16-bit strings of both byte orders under the rolling-XOR passes:

```bash
./hstrings --xor-roll -e l,b sample.bin
```

Read from standard input:

```bash
cat sample.bin | ./hstrings --shl
```

## Installation

```bash
sudo make install      # into /usr/local/bin
sudo make uninstall
```

`PREFIX`, `BINDIR` and `DESTDIR` are honoured, so packaging works as expected:

```bash
make install DESTDIR=/tmp/stage PREFIX=/usr
```

## Tests

```bash
make test
```

The suite covers every transform, the wide encodings, the minimum-length and
offset options, colour handling, ranking, thread-count independence, input
sources, and the exit statuses.
It needs `python3` to build the transformed fixtures.

## License

Copyright (C) 2023 Dimitrios Vlastaras. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
