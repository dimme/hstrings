/*
 * Copyright (C) 2023 Dimitrios Vlastaras. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define XOR_FLAG 1337
#define PRINTABLE_CHARS 3

// Function to rotate bits left or right, depending on the sign of 'bits'
unsigned char rotate_bits(unsigned char byte, int bits) {
    if (bits > 0) {
        return (byte << bits) | (byte >> (8 - bits));
    } else {
        bits = -bits;
        return (byte >> bits) | (byte << (8 - bits));
    }
}

// Process a buffer with a given rotation, and print sequences of printable characters
void process_buffer(unsigned char *buffer, size_t len, int rotation, unsigned char xor_key) {
    int is_prev_ch_printable = 0;
    int seq_len = 0;

    // Iterate through the buffer
    for (size_t pos = 0; pos < len; pos++) {
        unsigned char ch = buffer[pos];
        if (rotation != XOR_FLAG) {
            ch = rotate_bits(ch, rotation);
        } else {
            ch ^= xor_key;
        }

        // Check if the character is printable
        if (isprint(ch)) {
            seq_len++;
            if (seq_len >= PRINTABLE_CHARS) {
                if (!is_prev_ch_printable) {
                    putchar('\n');
                }
                putchar(ch);
                is_prev_ch_printable = 1;
            }
        } else {
            if (is_prev_ch_printable) {
                is_prev_ch_printable = 0;
            }
            seq_len = 0;
        }
    }

    putchar('\n');
}

int main(int argc, char *argv[]) {
    FILE *file;
    unsigned char *buffer;
    size_t buffer_size = 4096;
    size_t buffer_pos = 0;

    // Check for the correct number of arguments
    if (argc > 2) {
        printf("Usage: %s [binary_file]\n", argv[0]);
        return 1;
    }

    // Open the input file or use stdin
    if (argc == 2) {
        file = fopen(argv[1], "rb");
        if (file == NULL) {
            perror("Error opening file");
            return 1;
        }
    } else {
        file = stdin;
    }

    // Allocate memory for the buffer
    buffer = (unsigned char *)malloc(buffer_size);
    if (buffer == NULL) {
        perror("Error allocating memory");
        return 1;
    }

    // Read the file into the buffer
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        buffer[buffer_pos++] = ch;
        if (buffer_pos >= buffer_size) {
            buffer_size *= 2;
            buffer = (unsigned char *)realloc(buffer, buffer_size);
            if (buffer == NULL) {
                perror("Error reallocating memory");
                return 1;
            }
        }
    }

    // Define the rotations to be applied, yes XOR is rotation, I'm lazy, deal with it
    int rotations[] = {4, 3, 2, 1, -1, -2, -3, XOR_FLAG};
    size_t num_rotations = sizeof(rotations) / sizeof(rotations[0]);

    // Iterate through the rotations and XOR and process the buffer for each one
    for (size_t i = 0; i < num_rotations; i++) {
        if (rotations[i] != XOR_FLAG) {
            if (rotations[i] > 0) {
                printf("\033[1;31mROL-%d:\033[0m", rotations[i]);
            } else {
                printf("\033[1;34mROR-%d:\033[0m", -rotations[i]);
            }
            process_buffer(buffer, buffer_pos, rotations[i], 0);
        } else {
            for (int xor_key_int = 0x00; xor_key_int <= 0xFF; xor_key_int++) {
                unsigned char xor_key = (unsigned char)xor_key_int;
                printf("\033[1;32mXOR-0x%02X:\033[0m", xor_key);
                process_buffer(buffer, buffer_pos, rotations[i], xor_key);
            }
        }
    }

    // Close the file if it was opened, and free the memory
    if (argc == 2) {
        fclose(file);
    }
    free(buffer);

    return 0;
}
