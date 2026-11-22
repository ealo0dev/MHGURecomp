#!/bin/sh
# NSP import test (runtime/nspimport.c). No game and no keys needed: tests/gen_test_nsp.py makes a made-up base game NSP and
# update NSP (the update's RomFS a BKTR patch of the base's) with made-up keys, the importer extracts them, and the result
# must equal what the generator wrote. With hactool on the PATH (or HACTOOL=...), hactool's own dump of the patched RomFS
# section must also equal the generator's: a second implementation of the AES-CTR and BKTR parts.
# Needs python3 with `cryptography` and a C compiler (CC, default cc).
set -e
cd "$(dirname "$0")/.."
W=${TMPDIR:-/tmp}/nsp-test.$$
trap 'rm -rf "$W"' EXIT
mkdir -p "$W"
CC=${CC:-cc}
$CC -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter tests/test_nspimport.c runtime/nspimport.c -o "$W/imp"
for cfg in "1" "2 --same-id" "4 --big" "5 --big --same-id"; do
    set -- $cfg
    seed=$1; shift
    d="$W/s$seed"
    python3 tests/gen_test_nsp.py "$d" --seed "$seed" "$@" > /dev/null
    for keys in prod.keys prod.direct.keys; do
        rm -rf "$d/out"; mkdir "$d/out"
        "$W/imp" "$d/$keys" "$d/out" "$d/update.nsp" "$d/base.nsp" > /dev/null 2>&1 || { echo "seed $seed $keys: import failed"; exit 1; }
        diff -r "$d/expected" "$d/out" > /dev/null || { echo "seed $seed $keys: the extracted files differ"; exit 1; }
    done
    H=${HACTOOL:-$(command -v hactool || true)}
    if [ -n "$H" ]; then
        "$H" -k "$d/prod.direct.keys" --titlekey="$(cat "$d/titlekey.enc")" --basenca="$d/base.nca" --raw --section1="$d/sec1.bin" "$d/update.nca" > /dev/null 2>&1 || true
        cmp "$d/sec1.bin" "$d/update.section1.expected" || { echo "seed $seed: hactool's patched section differs"; exit 1; }
    fi
    echo "seed $seed $*: OK"
done

# another game (as the extractor program takes them): its files come out in the folder it names, the games can't be mixed,
# and the port's own check (the importer's default) refuses it
python3 tests/gen_test_nsp.py "$W/x" --seed 8 --app-id 0100C3800049C000 --same-id > /dev/null
python3 tests/gen_test_nsp.py "$W/y" --seed 8 > /dev/null  # (same seed: the same keys, another game)
$CC -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter tools/nspextract.c runtime/nspimport.c -o "$W/ext"
mkdir -p "$W/xout"; cp "$W/ext" "$W/x/prod.keys" "$W/xout/"; cp "$W/x/base.nsp" "$W/x/update.nsp" "$W/xout/"
(cd "$W/xout" && MHGU_NSP_ANY_VERSION=1 ./ext < /dev/null > run.log 2>&1) || { echo "other game: extractor failed"; cat "$W/xout/run.log"; exit 1; }
diff -r "$W/x/expected" "$W/xout/data-mhxx" > /dev/null || { echo "other game: the extracted files differ"; exit 1; }
rm -rf "$W/o2"; mkdir "$W/o2"
if "$W/imp" "$W/x/prod.keys" "$W/o2" "$W/x/base.nsp" "$W/x/update.nsp" > "$W/o2.log" 2>&1; then echo "the port's importer took another game"; exit 1; fi
grep -q "another game" "$W/o2.log" || { echo "no clear message for another game"; exit 1; }
if (cd "$W/xout" && MHGU_NSP_ANY_VERSION=1 ./ext --out "$W/o3" "$W/x/base.nsp" "$W/y/update.nsp" < /dev/null > "$W/o3.log" 2>&1); then echo "mixed games were taken"; exit 1; fi
grep -q "different games" "$W/o3.log" || { echo "no clear message for mixed games"; exit 1; }
echo "other game / mixed games: OK"

# Super Smash Bros. Ultimate's extractor (tools/smashextract.c): its ids, its folder; MHGU's files are refused
python3 tests/gen_test_nsp.py "$W/s" --seed 21 --app-id 01006A800016E000 --same-id > /dev/null
$CC -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter tools/smashextract.c runtime/nspimport.c -o "$W/smash"
mkdir -p "$W/sout"; cp "$W/smash" "$W/s/prod.keys" "$W/s/base.nsp" "$W/s/update.nsp" "$W/sout/"
(cd "$W/sout" && ./smash < /dev/null > run.log 2>&1) || { echo "smash: extractor failed"; cat "$W/sout/run.log"; exit 1; }
diff -r "$W/s/expected" "$W/sout/smash-ultimate" > /dev/null || { echo "smash: the extracted files differ"; exit 1; }
if (cd "$W/xout" && "$W/smash" --keys "$W/x/prod.keys" --out "$W/o4" "$W/y/base.nsp" "$W/y/update.nsp" < /dev/null > "$W/o4.log" 2>&1); then echo "smash took MHGU"; exit 1; fi
grep -q "another game" "$W/o4.log" || { echo "smash: no clear message for another game"; exit 1; }
echo "smash: OK"

# the NSP merger (tools/nspmerge.c): base + update + DLC into one PFS0 with every file byte for byte, shared files once
M="$W/m"; rm -rf "$M"; mkdir -p "$M"
python3 - "$M" <<'PY'
import os, struct, sys
d = sys.argv[1]
def pfs(files, path):
    names = b''.join(n.encode() + b'\0' for n, _ in files)
    out = b'PFS0' + struct.pack('<III', len(files), len(names), 0); off = 0; ents = b''; so = 0
    for n, b in files:
        ents += struct.pack('<QQII', off, len(b), so, 0); off += len(b); so += len(n) + 1
    open(os.path.join(d, path), 'wb').write(out + ents + names + b''.join(b for _, b in files))
r = lambda n: os.urandom(n)
pfs([('a.nca', r(100000)), ('a.cnmt.nca', r(5000)), ('x.cert', b'C' * 700)], 'G [01006A800016E000][v0].nsp')
pfs([('u.nca', r(70000)), ('x.cert', b'C' * 700)], 'G [01006A800016E800][v65536].nsp')
pfs([('d.nca', r(30000))], 'G [01006A800016F001][v0].nsp')
PY
$CC -std=gnu17 -O1 -g -Wall -Wextra tools/nspmerge.c -o "$W/merge"
(cd "$M" && "$W/merge" G*E000*.nsp G*E800*.nsp G*F001*.nsp < /dev/null > run.log 2>&1) || { echo "merge failed"; cat "$M/run.log"; exit 1; }
python3 - "$M" <<'PY'
import glob, struct, sys
d = sys.argv[1]
def files(path):
    b = open(path, 'rb').read(); n, s = struct.unpack('<II', b[4:12]); tab = 16 + n * 24; nm = b[tab:tab + s]; out = {}
    for i in range(n):
        o, sz, so = struct.unpack('<QQI', b[16 + i * 24:36 + i * 24]); out[nm[so:].split(b'\0')[0]] = b[tab + s + o:tab + s + o + sz]
    return out, (tab + s) % 0x4000
want = {}
for f in glob.glob(d + '/G [0*.nsp'): want.update(files(f)[0])
got, pad = files(d + '/G [FULL].nsp')
assert got == want and pad == 0, "merged NSP differs"
PY
if (cd "$M" && "$W/merge" G*E000*.nsp "G [FULL].nsp" < /dev/null > run2.log 2>&1); then echo "merge overwrote/took a [FULL] twice"; exit 1; fi
head -c 50000 "$M/G [01006A800016E000][v0].nsp" > "$M/cut.nsp"
if "$W/merge" "$M/cut.nsp" "$M/G [01006A800016E800][v65536].nsp" --out "$M/o.nsp" > "$M/o.log" 2>&1; then echo "merge took a cut-off NSP"; exit 1; fi
grep -q "cut off" "$M/o.log" || { echo "merge: no clear message for a cut-off NSP"; exit 1; }
echo "merge: OK"
echo "test-nsp: OK"
