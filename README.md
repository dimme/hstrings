# hstrings

`hstrings` finds continuous sequences of printable characters hidden inside a
binary file or on standard input. Plain `strings` only sees text that is stored
verbatim; `hstrings` first transforms the input in a number of ways, so text
that was rotated, bit-shifted or XORed shows up too. The output is colour-coded
by transform.

## Transforms

| Header    | Transform                                                          |
| --------- | ------------------------------------------------------------------ |
| `ROL-N`   | each byte rotated left by N bits (red)                              |
| `ROR-N`   | each byte rotated right by N bits (blue)                            |
| `SHL-N`   | the whole bit stream shifted left by N bits (magenta)               |
| `XOR-0xNN`| every byte XORed with the key `0xNN` (green)                        |

`ROL`/`ROR` rotate each byte on its own, which is what you want when the data
was obfuscated byte by byte. `SHL` shifts the entire bit stream, which is what
you want when the text sits at an offset that is not a multiple of eight bits —
there the bits of one character are split across two input bytes, and a per-byte
rotation recovers only a garbled version of it. Shifting the stream right by N
bits yields the same characters as shifting it left by 8 - N, so only the left
direction is generated.

With no transform option, `hstrings` runs the seven distinct byte rotations
(`ROL-4` and `ROR-4` are the same rotation, so it is listed once), the seven bit
stream shifts, and all 256 XOR keys. A pass that finds nothing prints no header.

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

| Option              | Meaning                                                      |
| ------------------- | ------------------------------------------------------------ |
| `-n`, `--min-len=LEN` | print runs of at least LEN characters (default 3)          |
| `-t`, `--radix=RADIX` | print the offset of each run; RADIX is `d`, `x` or `o`     |
| `--color[=WHEN]`    | colourise headers; WHEN is `auto` (default), `always`, `never` |
| `--rol`             | run the `ROL-1`..`ROL-7` passes                              |
| `--ror`             | run the `ROR-1`..`ROR-7` passes                              |
| `--shl`             | run the `SHL-1`..`SHL-7` passes                              |
| `--xor[=KEY]`       | run every XOR key, or just KEY (e.g. `0x4F`)                 |
| `-h`, `--help`      | display help and exit                                        |
| `-V`, `--version`   | output version information and exit                          |

Colour is written only when standard output is a terminal, so piping into
`grep` or `less` gives clean text.

## Examples

Scan a file with every transform:

```bash
./hstrings sample.bin
```

```
ROL-4:
(sequence 1)
(sequence 2)
ROR-1:
(sequence 1)
XOR-0x4F:
(sequence 1)
```

The full sweep is 270 passes and produces a lot of output, so it is usually
worth narrowing it down. Longer runs only, with offsets, from a single
transform family:

```bash
./hstrings --xor -n 8 -t x sample.bin
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

The suite covers the transforms, the minimum-length and offset options, colour
handling, input sources, and the exit statuses. It needs `python3` to build the
transformed fixtures.

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
