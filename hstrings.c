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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HSTRINGS_VERSION "1.2.0"

#define DEFAULT_MIN_LEN   4
#define MAX_MIN_LEN       (1024u * 1024u)
#define MAX_KEY_LEN       64
#define DEFAULT_GUESS_LEN 8
#define DEFAULT_TOP       100

#define COLOR_RESET "\033[0m"

/* ------------------------------------------------------------------------- */
/* Transforms                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Every pass is an operation that is undone on the whole input before the
 * result is searched for strings.  The labels name the *decoding* operation.
 *
 *   XOR-0xKK        every byte XORed with KK
 *   ADD-0xKK        KK added to every byte; undoes a SUB, and ADD-(256-KK)
 *                   undoes an ADD, so 255 keys cover both
 *   ROL-N / ROR-N   each byte rotated on its own
 *   SHL-N           the whole bit stream shifted left by N bits, for text
 *                   stored at an offset that is not a multiple of eight bits.
 *                   A right shift by N is a left shift by 8-N one byte
 *                   earlier, so only the left direction is generated.
 *   BITREV          the bit order of every byte reversed
 *   XORINC-0xKK     rolling XOR: byte i XORed with KK + i
 *   XORDEC-0xKK     rolling XOR: byte i XORed with KK - i
 *   XORCHAIN        byte i XORed with the previous input byte
 *   XORKEY-HEX      repeating multi-byte XOR key
 *   ROL-N+XOR-0xKK  rotate, then XOR.  XOR-then-rotate is the same set of
 *                   passes with a different key, so one order is enough.
 *   BASE64 / HEX    runs of encoded text in the raw input, decoded
 */
enum op {
    OP_XOR, OP_ROL, OP_ROR, OP_SHL, OP_BITREV, OP_ADD,
    OP_XORINC, OP_XORDEC, OP_XORCHAIN, OP_XORKEY, OP_ROTXOR,
    OP_BASE64, OP_HEX
};

struct pass {
    enum op op;
    unsigned param;                 /* bit count or key */
    unsigned param2;                /* XOR key for OP_ROTXOR */
    unsigned char key[MAX_KEY_LEN]; /* OP_XORKEY */
    size_t key_len;
    char label[64];
    const char *color;
};

static const char *op_color(enum op op)
{
    switch (op) {
    case OP_XOR:      return "\033[1;32m";
    case OP_ROL:      return "\033[1;31m";
    case OP_ROR:      return "\033[1;34m";
    case OP_SHL:      return "\033[1;35m";
    case OP_BITREV:   return "\033[1;37m";
    case OP_ADD:      return "\033[1;33m";
    case OP_XORINC:
    case OP_XORDEC:
    case OP_XORCHAIN: return "\033[1;36m";
    case OP_XORKEY:   return "\033[0;32m";
    case OP_ROTXOR:   return "\033[0;31m";
    case OP_BASE64:
    case OP_HEX:      return "\033[0;33m";
    }
    return "";
}

static unsigned char rotl8(unsigned char b, unsigned n)
{
    return (unsigned char)((b << n) | (b >> (8 - n)));
}

static unsigned char rotr8(unsigned char b, unsigned n)
{
    return (unsigned char)((b >> n) | (b << (8 - n)));
}

static unsigned char bitrev8(unsigned char b)
{
    b = (unsigned char)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (unsigned char)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (unsigned char)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* Undo the pass on the whole buffer.  Rotation and shift amounts are checked
   here so that a shift by 8 or more, which is undefined, can never happen. */
static void apply_pass(const struct pass *p, const unsigned char *in,
                       size_t len, unsigned char *out)
{
    size_t i;
    unsigned n = p->param;

    switch (p->op) {
    case OP_XOR:
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] ^ n);
        break;
    case OP_ADD:
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] + n);
        break;
    case OP_ROL:
        if (n == 0 || n > 7) { memcpy(out, in, len); break; }
        for (i = 0; i < len; i++) out[i] = rotl8(in[i], n);
        break;
    case OP_ROR:
        if (n == 0 || n > 7) { memcpy(out, in, len); break; }
        for (i = 0; i < len; i++) out[i] = rotr8(in[i], n);
        break;
    case OP_SHL:
        if (n == 0 || n > 7) { memcpy(out, in, len); break; }
        for (i = 0; i < len; i++) {
            unsigned char next = (i + 1 < len) ? in[i + 1] : 0u;
            out[i] = (unsigned char)((in[i] << n) | (next >> (8 - n)));
        }
        break;
    case OP_BITREV:
        for (i = 0; i < len; i++) out[i] = bitrev8(in[i]);
        break;
    case OP_XORINC:
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] ^ (n + i));
        break;
    case OP_XORDEC:
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] ^ (n - i));
        break;
    case OP_XORCHAIN:
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] ^ (i ? in[i - 1] : 0));
        break;
    case OP_XORKEY:
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] ^ p->key[i % p->key_len]);
        break;
    case OP_ROTXOR:
        if (n == 0 || n > 7) { memcpy(out, in, len); break; }
        for (i = 0; i < len; i++) out[i] = (unsigned char)(rotl8(in[i], n) ^ p->param2);
        break;
    case OP_BASE64:
    case OP_HEX:
        memcpy(out, in, len);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Encodings                                                                  */
/* ------------------------------------------------------------------------- */

enum enc { ENC_8, ENC_16LE, ENC_16BE, ENC_32LE, ENC_32BE, ENC_COUNT };

static const char *const enc_suffix[ENC_COUNT] = {
    "", "/16LE", "/16BE", "/32LE", "/32BE"
};
static const size_t enc_unit[ENC_COUNT] = { 1, 2, 2, 4, 4 };
static const char enc_letter[ENC_COUNT] = { 's', 'l', 'b', 'L', 'B' };

static int is_printable(unsigned char ch)
{
    /* Deliberately not isprint(): that follows the locale, and under a UTF-8
       locale it accepts bytes >= 0x80 that are not printable on their own. */
    return ch >= 0x20 && ch <= 0x7E;
}

/* The character encoded at p, or -1 if the unit is not a printable one. */
static int decode_unit(const unsigned char *p, enum enc e)
{
    switch (e) {
    case ENC_8:
        return is_printable(p[0]) ? p[0] : -1;
    case ENC_16LE:
        return (p[1] == 0 && is_printable(p[0])) ? p[0] : -1;
    case ENC_16BE:
        return (p[0] == 0 && is_printable(p[1])) ? p[1] : -1;
    case ENC_32LE:
        return (p[1] == 0 && p[2] == 0 && p[3] == 0 && is_printable(p[0])) ? p[0] : -1;
    case ENC_32BE:
        return (p[0] == 0 && p[1] == 0 && p[2] == 0 && is_printable(p[3])) ? p[3] : -1;
    case ENC_COUNT:
        break;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Readability score                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Cost, in half-bits, of each letter following each other letter in English:
 * -log2 P(next | previous), row-major a..z, quantised to 0..17.5 bits and
 * written as base-36 digits.  Derived from ~40 MB of English prose and source
 * code comments.  Running text averages 3 to 4 bits per pair; random letters
 * average about 7.5.
 */
static const char bigram_cost[26 * 26 + 1] =
    "LA89HFBM9NE586Q9Q675CDDFDN7FBD5GJK87M6FH7DN9AF6OPO7H"
    "6JDG6GK7AO88FI4GJAC6AIKODQ8EE92FIJ5KMAIG7HNA9D9KKIEM"
    "9F88B9EJGOG6A6GBE576IDE9DN7HDC88IL4SP9HF4EM7B98OGPEW"
    "AIGI4JB87OP9D7BGZ75C8FKQMG5NKI2LPK7ZIGGH6MRBF9DQOTGP"
    "CC9BB7AQIQH8948CNB66MCREVCAUEJ1LUNCNEKNN5CUIAL7UNUIU"
    "9JED2FDG6LK9I9DDIC7FCI8RIL6GHA45JM6QL7JG7FUH9A9JJRBM"
    "4AJE3FNM9TJDBI76ZFAEBNGSIM7J866C6JAODDIC6EKJ86BCKQEP"
    "EA97G9DNDQDA94B9U5988DBENK5LFE5GKE7ZI7JJ68M6C7AGHRAY"
    "GJIHJLOPJMUCMEFEGDCI0MOKUU7HCC4FAL7RDEB86GX988BEHOAW"
    "BMBI3HEA7OEDBH99IG759JGMBQ7JDE5FO56RNEGI8DP7A88KFKAJ"
    "BACE6BDRBWL686D8U576IPTJSP3KGN2ILK7ZNKFHCJRJIJGMJNPP"
    "5IHB7HM63QQCM97JL7AIJLFJTN794979MH8YRDDMG7WHE4JKLBCJ"
    "FDCF8EGH9YPBA894O855GGBLHI8MIJ1FND6UUCCE6EUIHIKUKPFJ";

static float pair_bits(int a, int b)
{
    char c = bigram_cost[a * 26 + b];
    int v = (c >= 'A') ? c - 'A' + 10 : c - '0';
    return (float)v / 2.0f;
}

/* Substrings that mark a string as interesting to an analyst regardless of
   how English it looks: URLs, paths, DLL names, registry keys, shells. */
static const char *const indicators[] = {
    "http", "www.", ".com", ".net", ".org", ".php", ".dll", ".exe", ".sys",
    ".bat", ".ps1", "c:\\", "\\\\", "hkey", "/bin/", "/etc/", "/tmp/",
    "passw", "user-agent", "cmd.exe", "powershell", NULL
};

static unsigned char lower_ascii(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static int has_indicator(const unsigned char *s, size_t n)
{
    size_t i, j, k, m;

    for (k = 0; indicators[k] != NULL; k++) {
        const char *ind = indicators[k];
        m = strlen(ind);
        if (m > n) continue;
        for (i = 0; i + m <= n; i++) {
            for (j = 0; j < m; j++) {
                if (lower_ascii(s[i + j]) != (unsigned char)ind[j]) break;
            }
            if (j == m) return 1;
        }
    }
    return 0;
}

static int is_letter(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static int is_punct_char(unsigned char c)
{
    return strchr(".,:;'\"-_/\\()?!@%&=+#*[]<>", c) != NULL && c != '\0';
}

/* Punctuation pairs that occur in real strings: URLs, paths, operators. */
static int punct_pair_ok(unsigned char a, unsigned char b)
{
    static const char *const ok[] = {
        "//", "\\\\", "::", "..", ":/", ":\\", "--", "__", "==", NULL
    };
    size_t k;
    if (b == '%') return 1;
    for (k = 0; ok[k] != NULL; k++) {
        if (a == (unsigned char)ok[k][0] && b == (unsigned char)ok[k][1]) return 1;
    }
    return 0;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float powf_int(float base, size_t exp)
{
    float r = 1.0f;
    while (exp-- > 0) r *= base;
    return r;
}

/*
 * How much a run of printable characters looks like something a person wrote,
 * on a scale of 0 to 1.  The main signal is how likely each pair of adjacent
 * letters is in English; that is blended with how much of the run is letters,
 * spaces and ordinary punctuation, whether the vowel ratio is in the range
 * prose has, whether it is made of words, and how long it is.  Runs that are
 * mostly symbols, mostly one repeated character, mostly non-letters, that
 * flip case in the middle of words, or that string punctuation together are
 * marked down.  Strings that contain a URL, path, DLL name or similar are
 * marked up.
 */
static float readability_span(const unsigned char *s, size_t n)
{
    size_t letters = 0, digits = 0, spaces = 0, punct = 0, other = 0;
    size_t vowels = 0, pairs = 0, repeats = 0, repeats2 = 0, flips = 0, clusters = 0;
    size_t words = 0, wordlen = 0, wordchars = 0;
    float bits = 0.0f;
    int prev_letter = -1;       /* 0..25, or -1 */
    int prev_lower = 0;
    unsigned char prev = 0, prev2 = 0;
    float text, bigram, vowel, structure, length, avg, score;
    size_t i;

    if (n == 0) return 0.0f;

    for (i = 0; i < n; i++) {
        unsigned char c = s[i];
        int lower = (c >= 'a' && c <= 'z');
        int upper = (c >= 'A' && c <= 'Z');

        if (i > 0 && c == prev) repeats++;
        if (i > 1 && c == prev2) repeats2++;
        if (i > 0 && is_punct_char(prev) && is_punct_char(c) && !punct_pair_ok(prev, c)) clusters++;
        prev2 = prev;
        prev = c;

        if (lower || upper) {
            int idx = lower ? c - 'a' : c - 'A';
            letters++;
            wordlen++;
            if (idx == 0 || idx == 4 || idx == 8 || idx == 14 || idx == 20) vowels++;
            if (prev_letter >= 0) {
                pairs++;
                bits += pair_bits(prev_letter, idx);
                if (prev_lower && upper && i + 1 < n && s[i + 1] >= 'A' && s[i + 1] <= 'Z') flips++;
            }
            prev_letter = idx;
            prev_lower = lower;
            continue;
        }

        prev_letter = -1;
        prev_lower = 0;

        if (is_digit(c)) {
            digits++;
            wordlen++;
        } else {
            if (wordlen >= 3) { words++; wordchars += wordlen; }
            wordlen = 0;
            if (c == ' ')                spaces++;
            else if (is_punct_char(c))   punct++;
            else                         other++;
        }
    }
    if (wordlen >= 3) { words++; wordchars += wordlen; }

    text = ((float)letters + (float)spaces + 0.6f * (float)digits
            + 0.5f * (float)punct) / (float)n;

    /* Shrink the per-pair average toward 6 bits so a lucky pair or two in a
       short run does not read as English. */
    avg = (bits + 2.0f * 6.0f) / ((float)pairs + 2.0f);
    bigram = clampf((7.0f - avg) / 3.5f, 0.0f, 1.0f);

    if (letters >= 3) {
        float r = (float)vowels / (float)letters;
        vowel = clampf(1.0f - (r > 0.38f ? r - 0.38f : 0.38f - r) / 0.38f, 0.0f, 1.0f);
    } else {
        vowel = 0.3f;
    }

    structure = 0.5f * ((float)wordchars / (float)n)
              + 0.5f * (words >= 2 ? 1.0f : (words == 1 ? 0.6f : 0.0f));

    length = clampf((float)n / 24.0f, 0.0f, 1.0f);

    score = 0.20f * text + 0.35f * bigram + 0.10f * vowel
          + 0.15f * structure + 0.20f * length;

    score *= clampf(powf_int(0.7f, other), 0.15f, 1.0f);
    score *= clampf(powf_int(0.75f, clusters), 0.3f, 1.0f);
    if (repeats * 3 > n)                                   score *= 0.5f;
    if (repeats * 2 > n)                                   score *= 0.5f;
    if (repeats2 * 5 >= n * 2)                             score *= 0.5f;
    if (letters >= 4 && flips >= 2 && flips * 8 > letters) score *= 0.6f;
    if (letters == 0)                                      score *= 0.5f;
    if (letters * 2 < n)                                   score *= 0.7f;
    if (n >= 8 && has_indicator(s, n))                     score *= 1.25f;

    return clampf(score, 0.0f, 1.0f);
}

/*
 * A run often carries junk on one or both ends: the bytes next to a real
 * string frequently come out printable under the same transform.  So the
 * run is scored as a whole and also piecewise, split wherever two or more
 * symbols sit together, and the best piece wins.
 */
static float readability(const unsigned char *s, size_t n)
{
    float best = readability_span(s, n);
    size_t i = 0, seg_start = 0;

    while (i < n) {
        size_t j = i;
        while (j < n && !is_letter(s[j]) && !is_digit(s[j]) && s[j] != ' ') j++;
        if (j - i >= 2) {
            if (i - seg_start >= 4 && !(seg_start == 0 && i == n)) {
                float sc = readability_span(s + seg_start, i - seg_start);
                if (sc > best) best = sc;
            }
            seg_start = j;
            i = j;
            continue;
        }
        i++;
    }
    if (seg_start > 0 && n - seg_start >= 4) {
        float sc = readability_span(s + seg_start, n - seg_start);
        if (sc > best) best = sc;
    }
    return best;
}

/* ------------------------------------------------------------------------- */
/* Output                                                                     */
/* ------------------------------------------------------------------------- */

struct options {
    size_t min_len;
    char radix;                 /* 'd', 'x', 'o', or 0 to omit offsets */
    int use_color;
    int rank;
    size_t top;                 /* 0 = unlimited */
};

struct hit {
    float score;
    size_t offset;
    size_t count;
    size_t pass_idx;
    enum enc enc;
    uint64_t hash;
    unsigned char *text;
    size_t len;
};

struct emitter {
    const struct options *opt;
    const struct pass *passes;

    /* run being accumulated */
    unsigned char *run;
    size_t run_len, run_cap;

    /* direct mode */
    int header_done;

    /* rank mode */
    struct hit *hits;
    size_t n_hits, cap_hits, limit;
    size_t *table;              /* hit index + 1, 0 = empty */
    size_t table_size;
    float floor;                /* scores at or below this cannot make the cut */
    int oom;
};

static uint64_t fnv1a(const unsigned char *s, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void print_label(const struct emitter *em, const struct pass *p, enum enc e)
{
    if (em->opt->use_color) {
        printf("%s%s%s%s", p->color, p->label, enc_suffix[e], COLOR_RESET);
    } else {
        printf("%s%s", p->label, enc_suffix[e]);
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

static int table_rebuild(struct emitter *em)
{
    size_t want = 16, i;

    while (want < em->cap_hits * 2) want *= 2;
    if (want != em->table_size) {
        size_t *t = realloc(em->table, want * sizeof(*t));
        if (t == NULL) return -1;
        em->table = t;
        em->table_size = want;
    }
    memset(em->table, 0, em->table_size * sizeof(*em->table));

    for (i = 0; i < em->n_hits; i++) {
        size_t slot = (size_t)em->hits[i].hash & (em->table_size - 1);
        while (em->table[slot] != 0) slot = (slot + 1) & (em->table_size - 1);
        em->table[slot] = i + 1;
    }
    return 0;
}

static int hit_cmp(const void *a, const void *b)
{
    const struct hit *x = a, *y = b;
    if (x->score != y->score) return (x->score > y->score) ? -1 : 1;
    if (x->len != y->len) return (x->len > y->len) ? -1 : 1;
    if (x->pass_idx != y->pass_idx) return (x->pass_idx < y->pass_idx) ? -1 : 1;
    if (x->offset != y->offset) return (x->offset < y->offset) ? -1 : 1;
    return 0;
}

/* Keep only the best 'top' hits once the table has grown to its limit. */
static int hits_truncate(struct emitter *em)
{
    size_t i;

    qsort(em->hits, em->n_hits, sizeof(*em->hits), hit_cmp);
    for (i = em->opt->top; i < em->n_hits; i++) {
        free(em->hits[i].text);
    }
    em->n_hits = em->opt->top;
    em->floor = em->hits[em->n_hits - 1].score;
    return table_rebuild(em);
}

static void emit_ranked(struct emitter *em, size_t pass_idx, enum enc e,
                        size_t offset, const unsigned char *s, size_t n)
{
    uint64_t h = fnv1a(s, n);
    size_t slot;
    struct hit *hit;
    float score;

    if (em->oom) return;

    if (em->table_size > 0) {
        slot = (size_t)h & (em->table_size - 1);
        while (em->table[slot] != 0) {
            hit = &em->hits[em->table[slot] - 1];
            if (hit->hash == h && hit->len == n && memcmp(hit->text, s, n) == 0) {
                hit->count++;
                return;
            }
            slot = (slot + 1) & (em->table_size - 1);
        }
    }

    score = readability(s, n);
    if (em->opt->top > 0 && em->n_hits >= em->opt->top && score <= em->floor) {
        return;
    }

    if (em->n_hits == em->cap_hits) {
        if (em->opt->top > 0 && em->n_hits >= em->limit) {
            if (hits_truncate(em) != 0) { em->oom = 1; return; }
        }
        if (em->n_hits == em->cap_hits) {
            size_t new_cap = em->cap_hits ? em->cap_hits * 2 : 256;
            struct hit *grown = realloc(em->hits, new_cap * sizeof(*grown));
            if (grown == NULL) { em->oom = 1; return; }
            em->hits = grown;
            em->cap_hits = new_cap;
            if (table_rebuild(em) != 0) { em->oom = 1; return; }
        }
    }

    hit = &em->hits[em->n_hits];
    hit->text = malloc(n);
    if (hit->text == NULL) { em->oom = 1; return; }
    memcpy(hit->text, s, n);
    hit->len = n;
    hit->score = score;
    hit->offset = offset;
    hit->count = 1;
    hit->pass_idx = pass_idx;
    hit->enc = e;
    hit->hash = h;
    em->n_hits++;

    slot = (size_t)h & (em->table_size - 1);
    while (em->table[slot] != 0) slot = (slot + 1) & (em->table_size - 1);
    em->table[slot] = em->n_hits;
}

/* A complete run of at least min_len printable characters was found. */
static void emit(struct emitter *em, size_t pass_idx, enum enc e,
                 size_t offset, const unsigned char *s, size_t n)
{
    if (em->opt->rank) {
        emit_ranked(em, pass_idx, e, offset, s, n);
        return;
    }

    if (!em->header_done) {
        print_label(em, &em->passes[pass_idx], e);
        printf(":\n");
        em->header_done = 1;
    }
    if (em->opt->radix) {
        print_offset(offset, em->opt->radix);
    }
    fwrite(s, 1, n, stdout);
    putchar('\n');
}

static int run_push(struct emitter *em, unsigned char c)
{
    if (em->run_len == em->run_cap) {
        size_t new_cap = em->run_cap ? em->run_cap * 2 : 256;
        unsigned char *grown = realloc(em->run, new_cap);
        if (grown == NULL) return -1;
        em->run = grown;
        em->run_cap = new_cap;
    }
    em->run[em->run_len++] = c;
    return 0;
}

static void run_flush(struct emitter *em, size_t pass_idx, enum enc e, size_t start)
{
    if (em->run_len >= em->opt->min_len) {
        emit(em, pass_idx, e, start, em->run, em->run_len);
    }
    em->run_len = 0;
}

/* Find every run of printable characters in buf, read as encoding e.  Wide
   encodings are tried at every byte alignment. */
static int scan(struct emitter *em, size_t pass_idx, const unsigned char *buf,
                size_t len, enum enc e)
{
    size_t unit = enc_unit[e];
    size_t a, pos, start = 0;

    em->header_done = 0;

    for (a = 0; a < unit; a++) {
        em->run_len = 0;
        for (pos = a; pos + unit <= len; pos += unit) {
            int ch = decode_unit(buf + pos, e);
            if (ch < 0) {
                run_flush(em, pass_idx, e, start);
                continue;
            }
            if (em->run_len == 0) start = pos;
            if (run_push(em, (unsigned char)ch) != 0) return -1;
        }
        run_flush(em, pass_idx, e, start);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Base64 and hex decoding                                                    */
/* ------------------------------------------------------------------------- */

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_text_byte(unsigned char c)
{
    return is_printable(c) || c == '\t' || c == '\n' || c == '\r';
}

/* Decoded output only counts if every byte of it is text and not all of it
   is blank; that is what keeps identifiers in the binary's own strings, which
   are valid base64 and hex too, from producing garbage. */
static int decoded_is_text(const unsigned char *s, size_t n, size_t min_len)
{
    size_t i, blank = 0;
    if (n < min_len) return 0;
    for (i = 0; i < n; i++) {
        if (!is_text_byte(s[i])) return 0;
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') blank++;
    }
    return blank < n;
}

static int scan_base64(struct emitter *em, size_t pass_idx,
                       const unsigned char *buf, size_t len)
{
    size_t min_run = (em->opt->min_len * 4 + 2) / 3;
    size_t pos = 0;

    if (min_run < 8) min_run = 8;
    em->header_done = 0;

    while (pos < len) {
        size_t start = pos, end, n, i, out_n = 0;
        unsigned acc = 0;
        int bits = 0;

        while (pos < len && b64_value(buf[pos]) >= 0) pos++;
        end = pos;
        while (pos < len && buf[pos] == '=' && pos - end < 2) pos++;
        n = end - start;

        if (n < min_run || (n & 3) == 1) {
            if (pos == start) pos++;
            continue;
        }

        em->run_len = 0;
        for (i = start; i < end; i++) {
            acc = (acc << 6) | (unsigned)b64_value(buf[i]);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (run_push(em, (unsigned char)((acc >> bits) & 0xFF)) != 0) return -1;
                out_n++;
            }
        }

        if (decoded_is_text(em->run, out_n, em->opt->min_len)) {
            emit(em, pass_idx, ENC_8, start, em->run, out_n);
        }
        em->run_len = 0;
    }
    return 0;
}

static int scan_hex(struct emitter *em, size_t pass_idx,
                    const unsigned char *buf, size_t len)
{
    size_t min_run = em->opt->min_len * 2;
    size_t pos = 0;

    if (min_run < 8) min_run = 8;
    em->header_done = 0;

    while (pos < len) {
        size_t start = pos, n, i, out_n = 0;

        while (pos < len && hex_value(buf[pos]) >= 0) pos++;
        n = pos - start;
        if (n < min_run) {
            if (pos == start) pos++;
            continue;
        }
        n &= ~(size_t)1;

        em->run_len = 0;
        for (i = 0; i < n; i += 2) {
            unsigned char b = (unsigned char)((hex_value(buf[start + i]) << 4)
                                              | hex_value(buf[start + i + 1]));
            if (run_push(em, b) != 0) return -1;
            out_n++;
        }

        if (decoded_is_text(em->run, out_n, em->opt->min_len)) {
            emit(em, pass_idx, ENC_8, start, em->run, out_n);
        }
        em->run_len = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Pass list                                                                  */
/* ------------------------------------------------------------------------- */

struct pass_list {
    struct pass *v;
    size_t n, cap;
};

static struct pass *pass_add(struct pass_list *pl, enum op op, unsigned param)
{
    struct pass *p;

    if (pl->n == pl->cap) {
        size_t new_cap = pl->cap ? pl->cap * 2 : 64;
        struct pass *grown = realloc(pl->v, new_cap * sizeof(*grown));
        if (grown == NULL) return NULL;
        pl->v = grown;
        pl->cap = new_cap;
    }

    p = &pl->v[pl->n++];
    memset(p, 0, sizeof(*p));
    p->op = op;
    p->param = param;
    p->color = op_color(op);

    switch (op) {
    case OP_XOR:      snprintf(p->label, sizeof(p->label), "XOR-0x%02X", param); break;
    case OP_ROL:      snprintf(p->label, sizeof(p->label), "ROL-%u", param); break;
    case OP_ROR:      snprintf(p->label, sizeof(p->label), "ROR-%u", param); break;
    case OP_SHL:      snprintf(p->label, sizeof(p->label), "SHL-%u", param); break;
    case OP_BITREV:   snprintf(p->label, sizeof(p->label), "BITREV"); break;
    case OP_ADD:      snprintf(p->label, sizeof(p->label), "ADD-0x%02X", param); break;
    case OP_XORINC:   snprintf(p->label, sizeof(p->label), "XORINC-0x%02X", param); break;
    case OP_XORDEC:   snprintf(p->label, sizeof(p->label), "XORDEC-0x%02X", param); break;
    case OP_XORCHAIN: snprintf(p->label, sizeof(p->label), "XORCHAIN"); break;
    case OP_BASE64:   snprintf(p->label, sizeof(p->label), "BASE64"); break;
    case OP_HEX:      snprintf(p->label, sizeof(p->label), "HEX"); break;
    case OP_XORKEY:   /* labelled by the caller once the key is known */
    case OP_ROTXOR:   break;
    }
    return p;
}

static int pass_add_range(struct pass_list *pl, enum op op, unsigned lo, unsigned hi)
{
    unsigned k;
    for (k = lo; k <= hi; k++) {
        if (pass_add(pl, op, k) == NULL) return -1;
    }
    return 0;
}

static int pass_add_xorkey(struct pass_list *pl, const unsigned char *key, size_t key_len)
{
    struct pass *p = pass_add(pl, OP_XORKEY, 0);
    size_t i, n = 0;

    if (p == NULL) return -1;
    memcpy(p->key, key, key_len);
    p->key_len = key_len;
    n = (size_t)snprintf(p->label, sizeof(p->label), "XORKEY-");
    for (i = 0; i < key_len && n + 3 <= sizeof(p->label); i++) {
        n += (size_t)snprintf(p->label + n, sizeof(p->label) - n, "%02X", key[i]);
    }
    return 0;
}

static int pass_add_rotxor(struct pass_list *pl)
{
    unsigned n, k;
    for (n = 1; n <= 7; n++) {
        for (k = 0; k <= 0xFF; k++) {
            struct pass *p = pass_add(pl, OP_ROTXOR, n);
            if (p == NULL) return -1;
            p->param2 = k;
            snprintf(p->label, sizeof(p->label), "ROL-%u+XOR-0x%02X", n, k);
        }
    }
    return 0;
}

/*
 * Guess repeating XOR keys of length 1..max_len.  Binaries are full of zero
 * bytes, so the most frequent byte at each key position is, more often than
 * not, the key byte itself.  Keys that are just a shorter key repeated are
 * skipped, as is any single-byte key when the full XOR sweep is already
 * in the list.
 */
static int pass_add_guesses(struct pass_list *pl, const unsigned char *buf,
                            size_t len, size_t max_len, int have_xor_sweep)
{
    static size_t counts[MAX_KEY_LEN][256];
    unsigned char key[MAX_KEY_LEN];
    size_t klen, j, i, d;

    if (len == 0) return 0;

    for (klen = 1; klen <= max_len && klen <= len; klen++) {
        int periodic = 0;

        memset(counts, 0, klen * sizeof(counts[0]));
        for (i = 0; i < len; i++) {
            counts[i % klen][buf[i]]++;
        }
        for (j = 0; j < klen; j++) {
            unsigned best = 0;
            for (i = 1; i < 256; i++) {
                if (counts[j][i] > counts[j][best]) best = (unsigned)i;
            }
            key[j] = (unsigned char)best;
        }

        for (d = 1; d < klen && !periodic; d++) {
            if (klen % d != 0) continue;
            periodic = 1;
            for (j = d; j < klen; j++) {
                if (key[j] != key[j - d]) { periodic = 0; break; }
            }
        }
        if (periodic) continue;

        if (klen == 1) {
            if (have_xor_sweep) continue;
            if (pass_add(pl, OP_XOR, key[0]) == NULL) return -1;
        } else {
            if (pass_add_xorkey(pl, key, klen) != 0) return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Input and arguments                                                        */
/* ------------------------------------------------------------------------- */

static const char *progname = "hstrings";

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
    if (buf == NULL) return NULL;

    for (;;) {
        size_t got;

        if (len == cap) {
            unsigned char *grown = realloc(buf, cap * 2);
            if (grown == NULL) { free(buf); return NULL; }
            buf = grown;
            cap *= 2;
        }

        got = fread(buf + len, 1, cap - len, f);
        len += got;

        if (got == 0) {
            if (ferror(f)) { free(buf); return NULL; }
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
"shifts, single and multi-byte XOR keys, rolling XOR, byte addition, bit\n"
"reversal, and base64 or hex encoding.  With no FILE, or when FILE is -,\n"
"read standard input.\n"
"\n"
"Output options:\n"
"  -n, --min-len=LEN     print runs of at least LEN characters (default %d)\n"
"  -t, --radix=RADIX     print the offset of each run; RADIX is d, x or o\n"
"  -e, --encoding=LIST   character encodings to look for, comma separated:\n"
"                        s (8-bit), l (16-bit LE), b (16-bit BE),\n"
"                        L (32-bit LE), B (32-bit BE); default s,l\n"
"      --color[=WHEN]    colourise labels; WHEN is auto (default), always\n"
"                        or never\n"
"      --rank            instead of listing by transform, collect every\n"
"                        string, drop duplicates, and print the ones most\n"
"                        likely to be human readable first\n"
"      --top=N           with --rank, print only the best N (default %d,\n"
"                        0 for all)\n"
"\n"
"Transform options (default: every one of them except --rotxor):\n"
"      --xor[=KEY]       every single-byte XOR key, or just KEY; KEY is a\n"
"                        byte (0x4F, 79) or a hex string (0xDEADBEEF) for a\n"
"                        repeating multi-byte key\n"
"      --xor-guess[=N]   guess repeating XOR keys up to N bytes long from\n"
"                        byte frequencies (default %d)\n"
"      --xor-roll        rolling XOR, key incrementing or decrementing per byte\n"
"      --xor-chain       XOR with the previous input byte\n"
"      --add             every byte-wise addition\n"
"      --rol, --ror      ROL-1..7 / ROR-1..7 byte rotations\n"
"      --shl             SHL-1..7 bit stream shifts\n"
"      --bitrev          bit order of each byte reversed\n"
"      --rotxor          every rotation combined with every XOR key\n"
"      --base64, --hex   decode runs of base64 / hex in the raw input\n"
"\n"
"  -h, --help            display this help and exit\n"
"  -V, --version         output version information and exit\n"
"\n"
"A pass that finds nothing prints no label.\n",
            progname, DEFAULT_MIN_LEN, DEFAULT_TOP, DEFAULT_GUESS_LEN);
}

static int parse_uint(const char *s, unsigned long *out, int base)
{
    char *end;
    unsigned long v;

    /* strtoul() silently accepts a leading minus and wraps it around, so
       reject the sign here rather than letting -1 arrive as ULONG_MAX. */
    if (s == NULL || *s == '\0' || *s == '-' || *s == '+') return -1;

    errno = 0;
    v = strtoul(s, &end, base);
    if (errno != 0 || *end != '\0') return -1;

    *out = v;
    return 0;
}

/* A key is either a decimal byte, a hex byte, or a longer hex string. */
static int parse_key(const char *s, unsigned char *key, size_t *key_len)
{
    unsigned long v;
    size_t n, i;

    if (s == NULL || *s == '\0') return -1;

    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
        s += 2;
        n = strlen(s);
        if (n == 0 || n > MAX_KEY_LEN * 2) return -1;
        if (n <= 2) {
            if (parse_uint(s, &v, 16) != 0) return -1;
            key[0] = (unsigned char)v;
            *key_len = 1;
            return 0;
        }
        if (n & 1) return -1;
        for (i = 0; i < n; i += 2) {
            int hi = hex_value((unsigned char)s[i]);
            int lo = hex_value((unsigned char)s[i + 1]);
            if (hi < 0 || lo < 0) return -1;
            key[i / 2] = (unsigned char)((hi << 4) | lo);
        }
        *key_len = n / 2;
        return 0;
    }

    if (parse_uint(s, &v, 10) != 0 || v > 0xFF) return -1;
    key[0] = (unsigned char)v;
    *key_len = 1;
    return 0;
}

static int parse_encodings(const char *s, int enabled[ENC_COUNT])
{
    int any = 0;

    memset(enabled, 0, ENC_COUNT * sizeof(*enabled));
    while (*s) {
        int e, found = 0;
        if (*s == ',') { s++; continue; }
        for (e = 0; e < ENC_COUNT; e++) {
            if (*s == enc_letter[e]) { enabled[e] = 1; found = 1; any = 1; }
        }
        if (!found) return -1;
        s++;
    }
    return any ? 0 : -1;
}

/* The value of an option given as "-x VALUE", "-xVALUE" or "--long=VALUE". */
static const char *option_value(int argc, char *argv[], int *i,
                                const char *arg, size_t long_prefix)
{
    const char *val = (arg[1] == '-') ? arg + long_prefix : arg + 2;

    if (*val == '\0') {
        if (*i + 1 >= argc) {
            fprintf(stderr, "%s: option '%s' requires an argument\n", progname, arg);
            return NULL;
        }
        val = argv[++*i];
    }
    return val;
}

struct selection {
    int xor, xor_guess, xor_roll, xor_chain, add, rol, ror, shl, bitrev;
    int rotxor, base64, hex;
    unsigned char key[MAX_KEY_LEN];
    size_t key_len;             /* 0 = every single-byte key */
    size_t guess_len;
};

static int build_passes(struct pass_list *pl, const struct selection *sel,
                        const unsigned char *buf, size_t len)
{
    int any = sel->xor || sel->xor_guess || sel->xor_roll || sel->xor_chain
           || sel->add || sel->rol || sel->ror || sel->shl || sel->bitrev
           || sel->rotxor || sel->base64 || sel->hex;
    int all = !any;
    int have_sweep = 0;

    if (all || sel->xor) {
        if (sel->key_len > 1) {
            if (pass_add_xorkey(pl, sel->key, sel->key_len) != 0) return -1;
        } else if (sel->key_len == 1) {
            if (pass_add(pl, OP_XOR, sel->key[0]) == NULL) return -1;
        } else {
            if (pass_add_range(pl, OP_XOR, 0x00, 0xFF) != 0) return -1;
            have_sweep = 1;
        }
    }
    if (all) {
        /* ROL-4 and ROR-4 are the same rotation, so the default set lists it
           once; --rol and --ror ask for a direction and get all seven. */
        unsigned k;
        for (k = 4; k >= 1; k--) if (pass_add(pl, OP_ROL, k) == NULL) return -1;
        if (pass_add_range(pl, OP_ROR, 1, 3) != 0) return -1;
    } else {
        if (sel->rol && pass_add_range(pl, OP_ROL, 1, 7) != 0) return -1;
        if (sel->ror && pass_add_range(pl, OP_ROR, 1, 7) != 0) return -1;
    }
    if ((all || sel->shl) && pass_add_range(pl, OP_SHL, 1, 7) != 0) return -1;
    if ((all || sel->bitrev) && pass_add(pl, OP_BITREV, 0) == NULL) return -1;
    if ((all || sel->add) && pass_add_range(pl, OP_ADD, 0x01, 0xFF) != 0) return -1;
    if (all || sel->xor_roll) {
        if (pass_add_range(pl, OP_XORINC, 0x00, 0xFF) != 0) return -1;
        if (pass_add_range(pl, OP_XORDEC, 0x00, 0xFF) != 0) return -1;
    }
    if ((all || sel->xor_chain) && pass_add(pl, OP_XORCHAIN, 0) == NULL) return -1;
    if (all || sel->xor_guess) {
        if (pass_add_guesses(pl, buf, len, sel->guess_len, have_sweep) != 0) return -1;
    }
    if (sel->rotxor && pass_add_rotxor(pl) != 0) return -1;
    if ((all || sel->base64) && pass_add(pl, OP_BASE64, 0) == NULL) return -1;
    if ((all || sel->hex) && pass_add(pl, OP_HEX, 0) == NULL) return -1;

    return 0;
}

static void print_ranked(struct emitter *em)
{
    size_t i, width = 8, shown = em->n_hits;

    qsort(em->hits, em->n_hits, sizeof(*em->hits), hit_cmp);
    if (em->opt->top > 0 && shown > em->opt->top) {
        shown = em->opt->top;
    }

    for (i = 0; i < shown; i++) {
        const struct hit *h = &em->hits[i];
        size_t w = strlen(em->passes[h->pass_idx].label) + strlen(enc_suffix[h->enc]);
        if (w > width) width = w;
    }

    for (i = 0; i < shown; i++) {
        const struct hit *h = &em->hits[i];
        const struct pass *p = &em->passes[h->pass_idx];
        size_t w = strlen(p->label) + strlen(enc_suffix[h->enc]);

        printf("%3d  ", (int)(h->score * 100.0f + 0.5f));
        print_label(em, p, h->enc);
        printf("%*s  ", (int)(width - w), "");
        if (em->opt->radix) {
            print_offset(h->offset, em->opt->radix);
        }
        fwrite(h->text, 1, h->len, stdout);
        if (h->count > 1) {
            printf("  [x%zu]", h->count);
        }
        putchar('\n');
    }
}

int main(int argc, char *argv[])
{
    struct options opt = { DEFAULT_MIN_LEN, 0, 0, 0, 0 };
    struct selection sel;
    struct pass_list pl = { NULL, 0, 0 };
    struct emitter em;
    int enabled[ENC_COUNT] = { 1, 1, 0, 0, 0 };
    unsigned char *buf = NULL;
    unsigned char *work = NULL;
    size_t len = 0;
    const char *path = NULL;
    FILE *file = stdin;
    int color_mode = 0;             /* 0 auto, 1 always, 2 never */
    int top_given = 0;
    int no_more_options = 0;
    int status = 1;
    int i;
    size_t p;

    memset(&sel, 0, sizeof(sel));
    sel.guess_len = DEFAULT_GUESS_LEN;
    memset(&em, 0, sizeof(em));
    em.opt = &opt;

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
        } else if (strncmp(arg, "--min-len=", 10) == 0 || (arg[1] == 'n' && arg[1] != '-')) {
            if ((val = option_value(argc, argv, &i, arg, 10)) == NULL) goto out;
            if (parse_uint(val, &n, 10) != 0 || n < 1 || n > MAX_MIN_LEN) {
                fprintf(stderr, "%s: invalid minimum length '%s'\n", progname, val);
                goto out;
            }
            opt.min_len = (size_t)n;
        } else if (strncmp(arg, "--radix=", 8) == 0 || arg[1] == 't') {
            if ((val = option_value(argc, argv, &i, arg, 8)) == NULL) goto out;
            if (val[1] != '\0' || (val[0] != 'd' && val[0] != 'x' && val[0] != 'o')) {
                fprintf(stderr, "%s: invalid radix '%s'; use d, x or o\n", progname, val);
                goto out;
            }
            opt.radix = val[0];
        } else if (strncmp(arg, "--encoding=", 11) == 0 || arg[1] == 'e') {
            if ((val = option_value(argc, argv, &i, arg, 11)) == NULL) goto out;
            if (parse_encodings(val, enabled) != 0) {
                fprintf(stderr, "%s: invalid encoding list '%s'; use s, l, b, L, B\n",
                        progname, val);
                goto out;
            }
        } else if (strcmp(arg, "--color") == 0) {
            color_mode = 1;
        } else if (strncmp(arg, "--color=", 8) == 0) {
            val = arg + 8;
            if (strcmp(val, "auto") == 0)        color_mode = 0;
            else if (strcmp(val, "always") == 0) color_mode = 1;
            else if (strcmp(val, "never") == 0)  color_mode = 2;
            else {
                fprintf(stderr, "%s: invalid colour mode '%s'\n", progname, val);
                goto out;
            }
        } else if (strcmp(arg, "--rank") == 0) {
            opt.rank = 1;
        } else if (strncmp(arg, "--top=", 6) == 0) {
            if (parse_uint(arg + 6, &n, 10) != 0) {
                fprintf(stderr, "%s: invalid count '%s'\n", progname, arg + 6);
                goto out;
            }
            opt.top = (size_t)n;
            top_given = 1;
        } else if (strcmp(arg, "--xor") == 0) {
            sel.xor = 1;
        } else if (strncmp(arg, "--xor=", 6) == 0) {
            if (parse_key(arg + 6, sel.key, &sel.key_len) != 0) {
                fprintf(stderr, "%s: invalid XOR key '%s'\n", progname, arg + 6);
                goto out;
            }
            sel.xor = 1;
        } else if (strcmp(arg, "--xor-guess") == 0) {
            sel.xor_guess = 1;
        } else if (strncmp(arg, "--xor-guess=", 12) == 0) {
            if (parse_uint(arg + 12, &n, 10) != 0 || n < 1 || n > MAX_KEY_LEN) {
                fprintf(stderr, "%s: invalid key length '%s' (1..%d)\n",
                        progname, arg + 12, MAX_KEY_LEN);
                goto out;
            }
            sel.xor_guess = 1;
            sel.guess_len = (size_t)n;
        } else if (strcmp(arg, "--xor-roll") == 0) {
            sel.xor_roll = 1;
        } else if (strcmp(arg, "--xor-chain") == 0) {
            sel.xor_chain = 1;
        } else if (strcmp(arg, "--add") == 0) {
            sel.add = 1;
        } else if (strcmp(arg, "--rol") == 0) {
            sel.rol = 1;
        } else if (strcmp(arg, "--ror") == 0) {
            sel.ror = 1;
        } else if (strcmp(arg, "--shl") == 0) {
            sel.shl = 1;
        } else if (strcmp(arg, "--bitrev") == 0) {
            sel.bitrev = 1;
        } else if (strcmp(arg, "--rotxor") == 0) {
            sel.rotxor = 1;
        } else if (strcmp(arg, "--base64") == 0) {
            sel.base64 = 1;
        } else if (strcmp(arg, "--hex") == 0) {
            sel.hex = 1;
        } else {
            fprintf(stderr, "%s: unrecognised option '%s'\n", progname, arg);
            usage(stderr);
            goto out;
        }
    }

    if (opt.rank && !top_given) opt.top = DEFAULT_TOP;
    if (top_given && !opt.rank) {
        fprintf(stderr, "%s: --top only makes sense with --rank\n", progname);
        goto out;
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

    work = malloc(len > 0 ? len : 1);
    if (work == NULL) goto nomem;

    if (build_passes(&pl, &sel, buf, len) != 0) goto nomem;
    em.passes = pl.v;
    em.limit = (opt.top * 2 > 1024) ? opt.top * 2 : 1024;

    switch (color_mode) {
    case 1:  opt.use_color = 1; break;
    case 2:  opt.use_color = 0; break;
    default: opt.use_color = isatty(STDOUT_FILENO) ? 1 : 0; break;
    }

    /* One big output buffer: the sweep writes well over a thousand passes. */
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);

    for (p = 0; p < pl.n; p++) {
        const struct pass *ps = &pl.v[p];
        int e;

        if (ps->op == OP_BASE64) {
            if (scan_base64(&em, p, buf, len) != 0) goto nomem;
            continue;
        }
        if (ps->op == OP_HEX) {
            if (scan_hex(&em, p, buf, len) != 0) goto nomem;
            continue;
        }

        apply_pass(ps, buf, len, work);
        for (e = 0; e < ENC_COUNT; e++) {
            if (enabled[e] && scan(&em, p, work, len, (enum enc)e) != 0) goto nomem;
        }
    }

    if (em.oom) goto nomem;
    if (opt.rank) print_ranked(&em);

    if (fflush(stdout) != 0) {
        fprintf(stderr, "%s: write error: %s\n", progname, strerror(errno));
        goto out;
    }

    status = 0;
    goto out;

nomem:
    fprintf(stderr, "%s: out of memory\n", progname);

out:
    if (file != NULL && file != stdin) fclose(file);
    for (p = 0; p < em.n_hits; p++) free(em.hits[p].text);
    free(em.hits);
    free(em.table);
    free(em.run);
    free(pl.v);
    free(work);
    free(buf);
    return status;
}
