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

#if !defined(_WIN32)
# define _POSIX_C_SOURCE 200809L
# if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
# endif
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HSTRINGS_VERSION "1.1.0"

#define DEFAULT_MIN_LEN 3
#define MAX_MIN_LEN     (1024u * 1024u)

#define COLOR_ROL   "\033[1;31m"
#define COLOR_ROR   "\033[1;34m"
#define COLOR_SHL   "\033[1;35m"
#define COLOR_XOR   "\033[1;32m"
#define COLOR_RESET "\033[0m"

/*
 * The transforms hstrings applies to the input before looking for strings.
 *
 * T_ROL / T_ROR rotate each byte on its own, which is what you want when the
 * data was obfuscated byte by byte.  T_SHL shifts the whole bit stream, which
 * is what you want when the text sits at an offset that is not a multiple of
 * eight bits: there the bits of a character are split across two input bytes.
 *
 * A right shift of the stream by k bits produces the same characters as a left
 * shift by 8 - k, just starting one byte earlier, so only the left direction is
 * generated.
 */
enum transform_kind { T_ROL, T_ROR, T_SHL, T_XOR };

struct pass {
    enum transform_kind kind;
    unsigned param;             /* bit count for ROL/ROR/SHL, key for XOR */
    char label[16];
    const char *color;
};

struct options {
    size_t min_len;
    char radix;                 /* 'd', 'x', 'o', or 0 to omit offsets */
    int use_color;
};

static const char *progname = "hstrings";

static int is_printable(unsigned char ch)
{
    /* Deliberately not isprint(): that follows the locale, and under a UTF-8
       locale it accepts bytes >= 0x80 that are not printable on their own. */
    return ch >= 0x20 && ch <= 0x7E;
}

static unsigned char pass_byte(const struct pass *p, const unsigned char *buf,
                               size_t len, size_t pos)
{
    unsigned char b = buf[pos];

    /* Shifting a byte by 8 or more is undefined, so refuse out-of-range
       amounts rather than trusting every caller to stay within 1..7. */
    if (p->kind != T_XOR && (p->param == 0 || p->param > 7)) {
        return b;
    }

    switch (p->kind) {
    case T_ROL:
        return (unsigned char)((b << p->param) | (b >> (8 - p->param)));
    case T_ROR:
        return (unsigned char)((b >> p->param) | (b << (8 - p->param)));
    case T_SHL: {
        unsigned char next = (pos + 1 < len) ? buf[pos + 1] : 0u;
        return (unsigned char)((b << p->param) | (next >> (8 - p->param)));
    }
    case T_XOR:
        return (unsigned char)(b ^ p->param);
    }

    return b;
}

static void print_header(const struct pass *p, const struct options *opt)
{
    if (opt->use_color) {
        printf("%s%s:%s\n", p->color, p->label, COLOR_RESET);
    } else {
        printf("%s:\n", p->label);
    }
}

static void print_offset(size_t off, char radix)
{
    switch (radix) {
    case 'x': printf("%7zx ", off); break;
    case 'o': printf("%7zo ", off); break;
    default:  printf("%7zu ", off); break;
    }
}

/*
 * Print every run of at least opt->min_len printable characters produced by
 * this pass.  The header is written lazily so that passes which find nothing
 * stay out of the output entirely.
 *
 * The first min_len - 1 characters of a run are held in 'hold' until the run is
 * known to be long enough; without that they would be dropped.
 */
static void scan_pass(const struct pass *p, const unsigned char *buf, size_t len,
                      const struct options *opt, unsigned char *hold)
{
    int header_done = 0;
    int printing = 0;
    size_t run = 0;
    size_t start = 0;

    for (size_t pos = 0; pos < len; pos++) {
        unsigned char ch = pass_byte(p, buf, len, pos);

        if (!is_printable(ch)) {
            if (printing) {
                putchar('\n');
                printing = 0;
            }
            run = 0;
            continue;
        }

        if (run == 0) {
            start = pos;
        }
        run++;

        if (run < opt->min_len) {
            hold[run - 1] = ch;
            continue;
        }

        if (run == opt->min_len) {
            if (!header_done) {
                print_header(p, opt);
                header_done = 1;
            }
            if (opt->radix) {
                print_offset(start, opt->radix);
            }
            fwrite(hold, 1, opt->min_len - 1, stdout);
            printing = 1;
        }

        putchar(ch);
    }

    if (printing) {
        putchar('\n');
    }
}

static int add_pass(struct pass **passes, size_t *count, size_t *cap,
                    enum transform_kind kind, unsigned param)
{
    struct pass *p;

    if (*count == *cap) {
        size_t new_cap = (*cap == 0) ? 16 : *cap * 2;
        struct pass *grown = realloc(*passes, new_cap * sizeof(**passes));
        if (grown == NULL) {
            return -1;
        }
        *passes = grown;
        *cap = new_cap;
    }

    p = &(*passes)[(*count)++];
    p->kind = kind;
    p->param = param;

    switch (kind) {
    case T_ROL:
        snprintf(p->label, sizeof(p->label), "ROL-%u", param);
        p->color = COLOR_ROL;
        break;
    case T_ROR:
        snprintf(p->label, sizeof(p->label), "ROR-%u", param);
        p->color = COLOR_ROR;
        break;
    case T_SHL:
        snprintf(p->label, sizeof(p->label), "SHL-%u", param);
        p->color = COLOR_SHL;
        break;
    case T_XOR:
        snprintf(p->label, sizeof(p->label), "XOR-0x%02X", param);
        p->color = COLOR_XOR;
        break;
    }

    return 0;
}

/* Read a whole stream.  Regular files are sized up front; pipes grow. */
static unsigned char *read_all(FILE *f, size_t *out_len)
{
    struct stat st;
    unsigned char *buf;
    size_t cap = 65536;
    size_t len = 0;

    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        cap = (size_t)st.st_size + 1;
    }

    buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }

    for (;;) {
        size_t got;

        if (len == cap) {
            unsigned char *grown = realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }

        got = fread(buf + len, 1, cap - len, f);
        len += got;

        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                return NULL;
            }
            break;
        }
    }

    *out_len = len;
    return buf;
}

static void usage(FILE *out)
{
    fprintf(out,
"Usage: %s [OPTION]... [FILE]\n"
"\n"
"Find runs of printable characters hidden behind bit rotations, bit stream\n"
"shifts and single-byte XOR keys.  With no FILE, or when FILE is -, read\n"
"standard input.\n"
"\n"
"Options:\n"
"  -n, --min-len=LEN     print runs of at least LEN characters (default %d)\n"
"  -t, --radix=RADIX     print the offset of each run; RADIX is d, x or o\n"
"      --color[=WHEN]    colourise headers; WHEN is auto (default), always\n"
"                        or never\n"
"      --rol             run the ROL-1..7 passes\n"
"      --ror             run the ROR-1..7 passes\n"
"      --shl             run the SHL-1..7 bit stream shift passes\n"
"      --xor[=KEY]       run every XOR key, or just KEY (e.g. 0x4F)\n"
"  -h, --help            display this help and exit\n"
"  -V, --version         output version information and exit\n"
"\n"
"With no transform option, every transform is applied: the seven distinct\n"
"byte rotations, the seven bit stream shifts, and all 256 XOR keys.  A pass\n"
"that finds nothing prints no header.\n",
            progname, DEFAULT_MIN_LEN);
}

static int parse_uint(const char *s, unsigned long *out, int base)
{
    char *end;
    unsigned long v;

    /* strtoul() silently accepts a leading minus and wraps it around, so
       reject the sign here rather than letting -1 arrive as ULONG_MAX. */
    if (s == NULL || *s == '\0' || *s == '-' || *s == '+') {
        return -1;
    }

    errno = 0;
    v = strtoul(s, &end, base);
    if (errno != 0 || *end != '\0') {
        return -1;
    }

    *out = v;
    return 0;
}

int main(int argc, char *argv[])
{
    struct options opt = { DEFAULT_MIN_LEN, 0, 0 };
    struct pass *passes = NULL;
    size_t pass_count = 0;
    size_t pass_cap = 0;
    unsigned char *buf = NULL;
    unsigned char *hold = NULL;
    size_t len = 0;
    const char *path = NULL;
    FILE *file = stdin;
    int want_rol = 0, want_ror = 0, want_shl = 0, want_xor = 0;
    long xor_key = -1;              /* -1 means every key */
    int color_mode = 0;             /* 0 auto, 1 always, 2 never */
    int no_more_options = 0;
    int status = 1;
    int i;

    if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0') {
        progname = argv[0];
    }

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *val;
        unsigned long n;

        if (no_more_options || arg[0] != '-' || arg[1] == '\0') {
            if (path != NULL) {
                fprintf(stderr, "%s: too many operands\n", progname);
                usage(stderr);
                goto out;
            }
            path = arg;
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            no_more_options = 1;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            status = 0;
            goto out;
        } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("hstrings %s\n", HSTRINGS_VERSION);
            status = 0;
            goto out;
        } else if (strncmp(arg, "--min-len=", 10) == 0 || arg[1] == 'n') {
            val = (arg[1] == 'n') ? arg + 2 : arg + 10;
            if (*val == '\0') {
                if (++i >= argc) {
                    fprintf(stderr, "%s: option '%s' requires an argument\n",
                            progname, arg);
                    goto out;
                }
                val = argv[i];
            }
            if (parse_uint(val, &n, 10) != 0 || n < 1 || n > MAX_MIN_LEN) {
                fprintf(stderr, "%s: invalid minimum length '%s'\n",
                        progname, val);
                goto out;
            }
            opt.min_len = (size_t)n;
        } else if (strncmp(arg, "--radix=", 8) == 0 || arg[1] == 't') {
            val = (arg[1] == 't') ? arg + 2 : arg + 8;
            if (*val == '\0') {
                if (++i >= argc) {
                    fprintf(stderr, "%s: option '%s' requires an argument\n",
                            progname, arg);
                    goto out;
                }
                val = argv[i];
            }
            if (val[1] != '\0' ||
                (val[0] != 'd' && val[0] != 'x' && val[0] != 'o')) {
                fprintf(stderr, "%s: invalid radix '%s'; use d, x or o\n",
                        progname, val);
                goto out;
            }
            opt.radix = val[0];
        } else if (strcmp(arg, "--color") == 0) {
            color_mode = 1;
        } else if (strncmp(arg, "--color=", 8) == 0) {
            val = arg + 8;
            if (strcmp(val, "auto") == 0) {
                color_mode = 0;
            } else if (strcmp(val, "always") == 0) {
                color_mode = 1;
            } else if (strcmp(val, "never") == 0) {
                color_mode = 2;
            } else {
                fprintf(stderr, "%s: invalid colour mode '%s'\n", progname, val);
                goto out;
            }
        } else if (strcmp(arg, "--rol") == 0) {
            want_rol = 1;
        } else if (strcmp(arg, "--ror") == 0) {
            want_ror = 1;
        } else if (strcmp(arg, "--shl") == 0) {
            want_shl = 1;
        } else if (strcmp(arg, "--xor") == 0) {
            want_xor = 1;
        } else if (strncmp(arg, "--xor=", 6) == 0) {
            if (parse_uint(arg + 6, &n, 0) != 0 || n > 0xFF) {
                fprintf(stderr, "%s: invalid XOR key '%s'\n", progname, arg + 6);
                goto out;
            }
            want_xor = 1;
            xor_key = (long)n;
        } else {
            fprintf(stderr, "%s: unrecognised option '%s'\n", progname, arg);
            usage(stderr);
            goto out;
        }
    }

    if (!want_rol && !want_ror && !want_shl && !want_xor) {
        unsigned k;

        /* ROL-4 and ROR-4 are the same rotation, so the default set lists it
           once; --rol and --ror ask for a direction explicitly and get all
           seven amounts. */
        for (k = 4; k >= 1; k--) {
            if (add_pass(&passes, &pass_count, &pass_cap, T_ROL, k) != 0) {
                goto nomem;
            }
        }
        for (k = 1; k <= 3; k++) {
            if (add_pass(&passes, &pass_count, &pass_cap, T_ROR, k) != 0) {
                goto nomem;
            }
        }
        for (k = 1; k <= 7; k++) {
            if (add_pass(&passes, &pass_count, &pass_cap, T_SHL, k) != 0) {
                goto nomem;
            }
        }
        for (k = 0; k <= 0xFF; k++) {
            if (add_pass(&passes, &pass_count, &pass_cap, T_XOR, k) != 0) {
                goto nomem;
            }
        }
    } else {
        unsigned k;

        if (want_rol) {
            for (k = 1; k <= 7; k++) {
                if (add_pass(&passes, &pass_count, &pass_cap, T_ROL, k) != 0) {
                    goto nomem;
                }
            }
        }
        if (want_ror) {
            for (k = 1; k <= 7; k++) {
                if (add_pass(&passes, &pass_count, &pass_cap, T_ROR, k) != 0) {
                    goto nomem;
                }
            }
        }
        if (want_shl) {
            for (k = 1; k <= 7; k++) {
                if (add_pass(&passes, &pass_count, &pass_cap, T_SHL, k) != 0) {
                    goto nomem;
                }
            }
        }
        if (want_xor) {
            if (xor_key >= 0) {
                if (add_pass(&passes, &pass_count, &pass_cap, T_XOR,
                             (unsigned)xor_key) != 0) {
                    goto nomem;
                }
            } else {
                for (k = 0; k <= 0xFF; k++) {
                    if (add_pass(&passes, &pass_count, &pass_cap, T_XOR, k) != 0) {
                        goto nomem;
                    }
                }
            }
        }
    }

    if (path != NULL && strcmp(path, "-") != 0) {
        file = fopen(path, "rb");
        if (file == NULL) {
            fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
            goto out;
        }
    }

    buf = read_all(file, &len);
    if (buf == NULL) {
        fprintf(stderr, "%s: %s: %s\n", progname,
                (path != NULL) ? path : "stdin", strerror(errno));
        goto out;
    }

    hold = malloc((opt.min_len > 1) ? opt.min_len - 1 : 1);
    if (hold == NULL) {
        goto nomem;
    }

    switch (color_mode) {
    case 1:  opt.use_color = 1; break;
    case 2:  opt.use_color = 0; break;
    default: opt.use_color = isatty(STDOUT_FILENO) ? 1 : 0; break;
    }

    /* One big output buffer: the XOR sweep alone writes 256 passes. */
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    for (size_t p = 0; p < pass_count; p++) {
        scan_pass(&passes[p], buf, len, &opt, hold);
    }

    if (fflush(stdout) != 0) {
        fprintf(stderr, "%s: write error: %s\n", progname, strerror(errno));
        goto out;
    }

    status = 0;
    goto out;

nomem:
    fprintf(stderr, "%s: out of memory\n", progname);

out:
    if (file != NULL && file != stdin) {
        fclose(file);
    }
    free(buf);
    free(hold);
    free(passes);
    return status;
}
