#!/bin/sh
#
# Golden-output tests for hstrings.  Usage: tests/run_tests.sh [path-to-binary]

set -u

HSTRINGS=${1:-./hstrings}
if [ ! -x "$HSTRINGS" ]; then
    echo "no hstrings binary at $HSTRINGS; run make first" >&2
    exit 1
fi
case "$HSTRINGS" in /*) ;; *) HSTRINGS="$PWD/$HSTRINGS" ;; esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
cd "$WORK" || exit 1

pass=0
fail=0

# check NAME EXPECTED ACTUAL
check() {
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1))
        printf 'ok   %s\n' "$1"
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n' "$1"
        printf '     expected: %s\n' "$(printf '%s' "$2" | tr '\n' '|')"
        printf '     actual:   %s\n' "$(printf '%s' "$3" | tr '\n' '|')"
    fi
}

# A string is printed in full.  Regression test: the scanner used to drop the
# first min_len - 1 characters of every run.
printf 'HelloWorld' > hello.bin
check "full string is printed" \
    "XOR-0x00:
HelloWorld" \
    "$("$HSTRINGS" --xor=0 hello.bin)"

# A run of exactly the minimum length is kept whole.
printf 'abc\000' > abc.bin
check "run of exactly min length" \
    "XOR-0x00:
abc" \
    "$("$HSTRINGS" --xor=0 abc.bin)"

# A run below the minimum length produces nothing, header included.
printf 'ab\000' > ab.bin
check "short run is dropped" "" "$("$HSTRINGS" --xor=0 ab.bin)"

check "-n lowers the threshold" \
    "XOR-0x00:
ab" \
    "$("$HSTRINGS" --xor=0 -n 2 ab.bin)"

check "--min-len= long form" \
    "XOR-0x00:
ab" \
    "$("$HSTRINGS" --xor=0 --min-len=2 ab.bin)"

# Headers are suppressed for passes that find nothing.
check "empty passes print no header" "" "$("$HSTRINGS" --rol --ror ab.bin)"

# Offsets.
printf '\000\000\000Hello\000' > off.bin
check "-t d prints decimal offsets" \
    "XOR-0x00:
      3 Hello" \
    "$("$HSTRINGS" --xor=0 -t d off.bin)"
printf '\000\000\000\000\000\000\000\000\000\000Hello' > off2.bin
check "-t x prints hex offsets" \
    "XOR-0x00:
      a Hello" \
    "$("$HSTRINGS" --xor=0 -t x off2.bin)"

# Colour is off when stdout is not a terminal, on when forced.
check "no colour when piped" "0" \
    "$("$HSTRINGS" --xor=0 hello.bin | grep -c "$(printf '\033')")"
check "--color=always emits escapes" "1" \
    "$("$HSTRINGS" --xor=0 --color=always hello.bin | grep -c "$(printf '\033')")"
check "--color=never overrides" "0" \
    "$("$HSTRINGS" --xor=0 --color=never hello.bin | grep -c "$(printf '\033')")"

# Byte rotations round-trip.
printf 'RotatedSecret' | tr -d '\n' > plain.bin
python3 -c "
import sys
d = open('plain.bin','rb').read()
open('rol3.bin','wb').write(bytes(((b >> 3) | (b << 5)) & 0xFF for b in d))
open('ror2.bin','wb').write(bytes(((b << 2) | (b >> 6)) & 0xFF for b in d))
open('xor5a.bin','wb').write(bytes(b ^ 0x5A for b in d))
n = int.from_bytes(d, 'big') << 3
open('shift.bin','wb').write(n.to_bytes(len(d) + 1, 'big'))
" || { echo "python3 required for the transform tests" >&2; exit 1; }

check "ROL pass recovers rotated text" \
    "ROL-3:
RotatedSecret" \
    "$("$HSTRINGS" --rol rol3.bin | grep -B1 RotatedSecret)"

check "ROR pass recovers rotated text" \
    "ROR-2:
RotatedSecret" \
    "$("$HSTRINGS" --ror ror2.bin | grep -B1 RotatedSecret)"

check "XOR pass recovers keyed text" \
    "XOR-0x5A:
RotatedSecret" \
    "$("$HSTRINGS" --xor=0x5A xor5a.bin)"

# A bit stream shifted by a non-multiple of eight is recovered exactly; the
# per-byte rotations only ever found a garbled version of it.
check "SHL pass recovers bit-shifted text" \
    "SHL-5:
RotatedSecret" \
    "$("$HSTRINGS" --shl shift.bin | grep -B1 RotatedSecret)"

# Input sources.
check "reads stdin" \
    "XOR-0x00:
HelloWorld" \
    "$("$HSTRINGS" --xor=0 < hello.bin)"
check "reads - as stdin" \
    "XOR-0x00:
HelloWorld" \
    "$(cat hello.bin | "$HSTRINGS" --xor=0 -)"
check "empty input produces no output" "" "$(printf '' | "$HSTRINGS")"

# Exit statuses and diagnostics.
"$HSTRINGS" --help > /dev/null 2>&1
check "--help exits 0" "0" "$?"
"$HSTRINGS" -V > /dev/null 2>&1
check "--version exits 0" "0" "$?"
"$HSTRINGS" /nonexistent-file > /dev/null 2>&1
check "missing file exits 1" "1" "$?"
check "missing file reports on stderr" "1" \
    "$("$HSTRINGS" /nonexistent-file 2>&1 >/dev/null | grep -c 'No such file')"
"$HSTRINGS" --bogus > /dev/null 2>&1
check "unknown option exits 1" "1" "$?"
"$HSTRINGS" -n 0 hello.bin > /dev/null 2>&1
check "-n 0 is rejected" "1" "$?"
"$HSTRINGS" -t q hello.bin > /dev/null 2>&1
check "invalid radix is rejected" "1" "$?"
"$HSTRINGS" --xor=0x100 hello.bin > /dev/null 2>&1
check "out-of-range XOR key is rejected" "1" "$?"
check "usage goes to stdout for --help" "1" \
    "$("$HSTRINGS" --help 2>/dev/null | grep -c '^Usage:')"

# Files whose names start with a dash are reachable after --.
printf 'HelloWorld' > ./-weird.bin
check "-- ends option parsing" \
    "XOR-0x00:
HelloWorld" \
    "$("$HSTRINGS" --xor=0 -- ./-weird.bin)"

# Bytes >= 0x80 are not printable regardless of locale.
printf 'ab\303\251cd\000' > utf8.bin
check "high bytes are not printable" "" \
    "$(LC_ALL=en_US.UTF-8 "$HSTRINGS" --xor=0 utf8.bin)"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
