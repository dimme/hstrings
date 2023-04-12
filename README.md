# hstrings

`hstrings` is a command-line tool for finding and displaying hidden continuous sequences of printable characters within a binary file or stdin input. The tool rotates the input bytes using various left-right rotations and XOR operations before searching for printable character sequences. The output is color-coded for each type of operation.

## Usage
To compile the program, simply run:
```bash
make
```

To use the program with a binary file, run:
```bash
./hstrings <binary_file>
```

To use the program with input from stdin, run:
```bash
./hstrings < input_file
```
or
```bash
cat input_file | ./hstrings
```

## Output
The program will print continuous sequences of printable characters that are 3 characters or longer on separate lines. The output is color-coded based on the type of rotation applied to the input bytes:

-   Red: Left rotations (ROL)
-   Blue: Right rotations (ROR)
-   Green: XOR operation

Before each set of sequences, the program prints the rotation type and the number of bits rotated, in the format `ROL-X`, `ROR-X` or `XOR-X` where `X` is the number of bits or key used.

## Example

Suppose you have a binary file named `sample.bin`. To use the `hstrings` tool on this file, run:
```bash
./hstrings sample.bin
```

The program will display the continuous sequences of printable characters for each rotation:
```
ROL-4:
(sequence 1)
(sequence 2)
...
ROR-1:
(sequence 1)
(sequence 2)
...
XOR-0x4F:
(sequence 1)
(sequence 2)
...
```

## Installation
To install the program, run:
```bash
make install
```

This command will install the `hstrings` tool to `/usr/local/bin`. You may need to run `sudo make install` to install with the required permissions.

To uninstall the program, run:
```bash
make uninstall
```

This command will remove the `hstrings` tool from `/usr/local/bin`. You may need to run `sudo make uninstall` to remove the program with the required permissions.

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
