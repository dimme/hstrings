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
printf 'abcd\000' > abcd.bin
check "run of exactly min length" \
    "XOR-0x00:
abcd" \
    "$("$HSTRINGS" --xor=0 abcd.bin)"

# A run below the minimum length produces nothing, header included.
printf 'abc\000' > abc.bin
printf 'ab\000' > ab.bin
check "short run is dropped" "" "$("$HSTRINGS" --xor=0 abc.bin)"

check "-n lowers the threshold" \
    "XOR-0x00:
abc" \
    "$("$HSTRINGS" --xor=0 -n 3 abc.bin)"

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
pad = b'\x00' * 8
open('add.bin','wb').write(pad + bytes((b - 0x1F) & 0xFF for b in d) + pad)
open('bitrev.bin','wb').write(pad + bytes(int(format(b, '08b')[::-1], 2) for b in d) + pad)
p = pad + d + pad
open('xorinc.bin','wb').write(bytes(b ^ ((0x10 + i) & 0xFF) for i, b in enumerate(p)))
open('xordec.bin','wb').write(bytes(b ^ ((0x10 - i) & 0xFF) for i, b in enumerate(p)))
c = bytearray(); prev = 0
for b in p:
    prev = b ^ prev; c.append(prev)
open('xorchain.bin','wb').write(c)
key = bytes.fromhex('DEADBEEF'); z = b'\x00' * 200
open('xorkey.bin','wb').write(bytes(b ^ key[i % 4] for i, b in enumerate(z + d + z)))
open('rotxor.bin','wb').write(pad + bytes((((b ^ 0x5A) >> 3) | ((b ^ 0x5A) << 5)) & 0xFF for b in d) + pad)
import base64
open('b64.bin','wb').write(b'\x00\x01' + base64.b64encode(b'Base64 hidden text!') + b'\x02\x00garbage')
open('hex.bin','wb').write(b'\x00' + b'4865782068696464656e' + b'\x00')
open('wide_le.bin','wb').write(pad + d.decode().encode('utf-16-le') + pad)
open('wide_be.bin','wb').write(pad + d.decode().encode('utf-16-be') + pad)
open('wide32le.bin','wb').write(pad + d.decode().encode('utf-32-le') + pad)
open('wide32be.bin','wb').write(pad + d.decode().encode('utf-32-be') + pad)
open('wide_xor.bin','wb').write(bytes(b ^ 0x33 for b in pad + d.decode().encode('utf-16-le') + pad))
import random
random.seed(7)
noise = bytes(random.getrandbits(8) for _ in range(4000))
open('mixed.bin','wb').write(noise[:2000] + b'kernel32.dll\x00' + bytes(b ^ 0x5A for b in b'http://evil.example.com/gate.php\x00')
    + b'This is a readable sentence hidden in the file.\x00' + b'x7#Kq~^|{}\x00' + b'AAAAAAAAAAAAAAAA\x00'
    + b'kernel32.dll\x00' + noise[2000:])
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

check "ADD pass recovers subtracted text" \
    "ADD-0x1F:
RotatedSecret" \
    "$("$HSTRINGS" --add add.bin | grep -B1 RotatedSecret)"

check "BITREV pass recovers bit-reversed text" \
    "BITREV:
RotatedSecret" \
    "$("$HSTRINGS" --bitrev bitrev.bin | grep -B1 RotatedSecret)"

check "XORINC pass recovers incrementing rolling XOR" \
    "XORINC-0x10:
RotatedSecret" \
    "$("$HSTRINGS" --xor-roll xorinc.bin | grep -B1 '^RotatedSecret$' | head -2)"

check "XORDEC pass recovers decrementing rolling XOR" \
    "XORDEC-0x10:
RotatedSecret" \
    "$("$HSTRINGS" --xor-roll xordec.bin | grep -B1 '^RotatedSecret$' | head -2)"

check "XORCHAIN pass recovers chained XOR" \
    "XORCHAIN:
RotatedSecret" \
    "$("$HSTRINGS" --xor-chain xorchain.bin | grep -B1 RotatedSecret)"

check "multi-byte --xor=KEY" \
    "XORKEY-DEADBEEF:
RotatedSecret" \
    "$("$HSTRINGS" --xor=0xDEADBEEF xorkey.bin | grep -B1 RotatedSecret)"

check "--xor-guess finds a 4-byte key from byte frequencies" \
    "XORKEY-DEADBEEF:
RotatedSecret" \
    "$("$HSTRINGS" --xor-guess xorkey.bin | grep -B1 RotatedSecret)"

check "--rotxor recovers rotate-then-XOR" \
    "ROL-3+XOR-0x5A:" \
    "$("$HSTRINGS" --rotxor -n 13 rotxor.bin | grep -B1 'ZZZZZZZZRotatedSecretZZZZZZZZ' | head -1)"

check "--base64 decodes an embedded base64 run" \
    "BASE64:
Base64 hidden text!" \
    "$("$HSTRINGS" --base64 b64.bin)"

check "--hex decodes an embedded hex run" \
    "HEX:
Hex hidden" \
    "$("$HSTRINGS" --hex hex.bin)"

check "identifiers are not mistaken for base64" "" \
    "$("$HSTRINGS" --base64 hello.bin)"

# Wide encodings.
check "16-bit LE strings are found by default" \
    "XOR-0x00/16LE:
RotatedSecret" \
    "$("$HSTRINGS" --xor=0 wide_le.bin)"
check "16-bit BE with -e b" \
    "XOR-0x00/16BE:
RotatedSecret" \
    "$("$HSTRINGS" --xor=0 -e b wide_be.bin)"
check "32-bit LE with -e L" \
    "XOR-0x00/32LE:
RotatedSecret" \
    "$("$HSTRINGS" --xor=0 -e L wide32le.bin)"
check "32-bit BE with -e B" \
    "XOR-0x00/32BE:
RotatedSecret" \
    "$("$HSTRINGS" --xor=0 -e B wide32be.bin)"
check "-e s excludes wide strings" "" "$("$HSTRINGS" --xor=0 -e s wide_le.bin)"
check "XORed 16-bit LE string is recovered" \
    "XOR-0x33/16LE:
RotatedSecret" \
    "$("$HSTRINGS" --xor wide_xor.bin | grep -B1 RotatedSecret)"

# Ranking.
# The three real strings in the fixture must be the top three, ahead of
# the thousands of printable runs the noise produces.
top3=$("$HSTRINGS" --rank --top=3 --xor mixed.bin)
check "--rank puts the URL in the top three" "1" \
    "$(printf '%s\n' "$top3" | grep -c 'http://evil.example.com/gate.php')"
check "--rank puts the DLL name in the top three" "1" \
    "$(printf '%s\n' "$top3" | grep -c 'kernel32.dll')"
check "--rank puts the sentence in the top three" "1" \
    "$(printf '%s\n' "$top3" | grep -c 'This is a readable sentence hidden in the file.')"
check "--rank drops duplicates and counts them" "1" \
    "$("$HSTRINGS" --rank --top=0 --xor=0 mixed.bin | grep -c 'kernel32.dll  \[x2\]')"
check "--rank scores junk low" "1" \
    "$("$HSTRINGS" --rank --top=0 --xor=0 mixed.bin | grep -E '^ *[0-9]+  ' | awk '/x7#Kq/ { print ($1 < 40) ? 1 : 0 }')"
check "--rank scores a repeated character low" "1" \
    "$("$HSTRINGS" --rank --top=0 --xor=0 mixed.bin | awk '/AAAAAAAAAAAAAAAA/ { print ($1 < 30) ? 1 : 0 }')"
check "--top limits the ranked list" "2" \
    "$("$HSTRINGS" --rank --top=2 --xor=0 mixed.bin | wc -l | tr -d ' ')"
check "--rank with -t shows offsets" "1" \
    "$("$HSTRINGS" --rank --top=1 --xor=0 -t d mixed.bin | grep -cE '^100  XOR-0x00 +[0-9]+ kernel32.dll')"

# Threads: the output must not depend on the thread count.
check "-j 1 and -j 3 give identical output" \
    "$("$HSTRINGS" -j 1 -t x mixed.bin | cksum)" \
    "$("$HSTRINGS" -j 3 -t x mixed.bin | cksum)"
check "-j 1 and -j 3 give identical ranked output" \
    "$("$HSTRINGS" -j 1 --rank --top=0 -t x mixed.bin | cksum)" \
    "$("$HSTRINGS" -j 3 --rank --top=0 -t x mixed.bin | cksum)"
check "-j 1 and -j 4 give identical ranked output with a small --top" \
    "$("$HSTRINGS" -j 1 --rank --top=5 mixed.bin | cksum)" \
    "$("$HSTRINGS" --threads=4 --rank --top=5 mixed.bin | cksum)"
check "-j 1 and -j 4 agree on all encodings and --rotxor" \
    "$("$HSTRINGS" -j 1 -e s,l,b,L,B --rotxor -n 6 rol3.bin | cksum)" \
    "$("$HSTRINGS" -j 4 -e s,l,b,L,B --rotxor -n 6 rol3.bin | cksum)"
check "more threads than passes is fine" \
    "XOR-0x5A:
RotatedSecret" \
    "$("$HSTRINGS" -j 64 --xor=0x5A xor5a.bin)"

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
"$HSTRINGS" --xor=256 hello.bin > /dev/null 2>&1
check "out-of-range XOR key is rejected" "1" "$?"
"$HSTRINGS" --xor=0x123 hello.bin > /dev/null 2>&1
check "odd-length hex XOR key is rejected" "1" "$?"
"$HSTRINGS" --top=5 hello.bin > /dev/null 2>&1
check "--top without --rank is rejected" "1" "$?"
"$HSTRINGS" -e q hello.bin > /dev/null 2>&1
check "invalid encoding letter is rejected" "1" "$?"
"$HSTRINGS" --xor-guess=0 hello.bin > /dev/null 2>&1
check "--xor-guess=0 is rejected" "1" "$?"
"$HSTRINGS" -j 0 hello.bin > /dev/null 2>&1
check "-j 0 is rejected" "1" "$?"
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
