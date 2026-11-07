#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host check of the codec: builds the library for this computer with cc, encodes a synthetic signal in every mode
# MeshKode can put on air (0..7: 3200, 2400, 1600, 1400, 1300, 1200, 700, 700B) and decodes it again.
#
#   tools/host_test.sh                  build, run, compare with tools/host_test.golden for this host
#   tools/host_test.sh --update         the same, and write this host's hashes into tools/host_test.golden
#   tools/host_test.sh --against DIR    A/B: build the same harness from DIR's copies of the same .c files (a
#                                       baseline checkout, upstream codec2, ...) and say, mode by mode, whether
#                                       the encoded bits and the decoded samples are identical
#
# Environment: CC (default cc), CFLAGS_EXTRA (added after the fixed flags), KEEP=1 to keep the work directory.
#
# Why both: the codec is float and calls libm (sin, cos, atan2, log10, pow...), and macOS, glibc and newlib do not
# round those alike, so hashes are only stable on one host and compiler, and the golden file keys them by both.
# Bit-exactness of a change is proved with --against: the old tree and the new one, same host, same flags.
# The flags pin what the codec computes: -O2, no fast-math, and no fused multiply-add (-ffp-contract=off).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
golden="$here/host_test.golden"
cc="${CC:-cc}"
cflags=(-std=gnu99 -O2 -fno-fast-math -ffp-contract=off -w ${CFLAGS_EXTRA:-})
modes=(0 1 2 3 4 5 6 7)
names=(3200 2400 1600 1400 1300 1200 700 700B)

update=0
against=""
while [ $# -gt 0 ]; do
    case "$1" in
        --update) update=1 ;;
        --against) against="${2:?--against needs a directory}"; shift ;;
        -h|--help) sed -n '3,19p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if command -v sha256sum >/dev/null 2>&1; then sha() { sha256sum "$1" | cut -d' ' -f1; }
else sha() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

work="$(mktemp -d "${TMPDIR:-/tmp}/codec2_host_test.XXXXXX")"
if [ "${KEEP:-0}" = 1 ]; then echo "work directory: $work"; else trap 'rm -rf "$work"' EXIT; fi

# The library's sources are this tree's src/*.c but dump.c (the debug dumps, which the component does not build
# either); --against takes the files of the same names from the other tree.
srcs=()
for f in "$root"/src/*.c; do
    case "$(basename "$f")" in dump.c) ;; *) srcs+=("$(basename "$f")") ;; esac
done

build() { # build <src dir> <out binary>
    local dir="$1" out="$2" list=() f
    for f in "${srcs[@]}"; do
        [ -f "$dir/$f" ] || { echo "$dir has no $f" >&2; exit 2; }
        list+=("$dir/$f")
    done
    "$cc" "${cflags[@]}" -I"$dir" "$here/host_test.c" "${list[@]}" -lm -o "$out"
}

run() { # run <binary> <tag>: one process per mode and direction, as codec2_rand is one per process
    local bin="$1" tag="$2" m
    for m in "${modes[@]}"; do
        "$bin" enc "$m" "$work/in.raw" "$work/$tag.$m.bit"
        "$bin" dec "$m" "$work/$tag.$m.bit" "$work/$tag.$m.raw"
    done
}

platform="$(uname -sm) | $("$cc" --version 2>/dev/null | head -1)"
echo "host: $platform"
echo "flags: ${cflags[*]}"

build "$root/src" "$work/ours"
"$work/ours" gen "$work/in.raw"
run "$work/ours" ours

fail=0

# The same build twice gives the same bytes.
"$work/ours" enc 0 "$work/in.raw" "$work/again.bit"
cmp -s "$work/again.bit" "$work/ours.0.bit" || { echo "FAIL: two encodes of mode 0 differ"; fail=1; }

# What every mode gives, and that it is speech-like and not silence.
printf '%-5s %-6s %6s %-16s %-16s %s\n' mode rate size bits pcm "decoded rms"
lines=()
for m in "${modes[@]}"; do
    bh="$(sha "$work/ours.$m.bit")"
    ph="$(sha "$work/ours.$m.raw")"
    nbytes="$(wc -c < "$work/ours.$m.bit" | tr -d ' ')"
    read -r _ _ _ snr rms _ <<< "$("$work/ours" cmp "$work/in.raw" "$work/ours.$m.raw")"
    read -r _ _ _ _ drms _ <<< "$("$work/ours" cmp "$work/ours.$m.raw" "$work/ours.$m.raw")"
    printf '%-5s %-6s %6s %-16s %-16s %s\n' "$m" "${names[$m]}" "$nbytes B" "${bh:0:16}" "${ph:0:16}" "rms $drms (input $rms)"
    awk -v r="$drms" 'BEGIN { exit !(r > 100) }' || { echo "FAIL: mode $m decodes to near silence"; fail=1; }
    lines+=("$platform	$m	$bh	$ph")
done
inhash="$(sha "$work/in.raw")"
echo "input signal: $inhash"

# The input is integer-made and the same everywhere.
if [ -f "$golden" ]; then
    gin="$(awk -F'\t' '$1 == "input" { print $2 }' "$golden")"
    if [ -n "$gin" ] && [ "$gin" != "$inhash" ]; then echo "FAIL: the synthetic signal changed"; fail=1; fi
fi

if [ "$update" = 1 ]; then
    {
        echo "# tools/host_test.sh hashes (sha256): platform | mode | encoded bits | decoded samples. Regenerate"
        echo "# with --update only for a change that is meant to alter the output, and say so in its commit."
        printf 'input\t%s\n' "$inhash"
        [ -f "$golden" ] && awk -F'\t' -v p="$platform" '$1 != "input" && $1 !~ /^#/ && $1 != p' "$golden"
        printf '%s\n' "${lines[@]}"
    } > "$golden.new"
    mv "$golden.new" "$golden"
    echo "golden: wrote $(printf '%s\n' "${lines[@]}" | wc -l | tr -d ' ') lines for this host"
elif [ -f "$golden" ] && grep -qF "$platform	" "$golden"; then
    for l in "${lines[@]}"; do
        if ! grep -qxF "$l" "$golden"; then
            echo "FAIL: mode $(echo "$l" | cut -f2) differs from tools/host_test.golden"
            fail=1
        fi
    done
    [ "$fail" = 0 ] && echo "golden: all ${#modes[@]} modes match this host's hashes"
else
    echo "golden: no hashes for this host and compiler; --update records them (output above not checked)"
fi

if [ -n "$against" ]; then
    adir="$(cd "$against" && pwd)"
    echo
    echo "A/B against $adir"
    build "$adir" "$work/theirs"
    run "$work/theirs" theirs
    printf '%-5s %-6s %-26s %-26s %s\n' mode rate "encoded bits" "decoded (same bits)" "decoded (own bits)"
    for m in "${modes[@]}"; do
        nbyte=$(( $(wc -c < "$work/ours.$m.bit") ))
        if cmp -s "$work/ours.$m.bit" "$work/theirs.$m.bit"; then benc="identical"
        else
            benc="$(cmp -l "$work/ours.$m.bit" "$work/theirs.$m.bit" 2>/dev/null | wc -l | tr -d ' ') bytes differ"
            fail=1
        fi
        # The other decoder on our bits isolates the decoder; its own bits show the whole chain.
        "$work/theirs" dec "$m" "$work/ours.$m.bit" "$work/cross.$m.raw"
        read -r nd _ me sn _ <<< "$("$work/ours" cmp "$work/ours.$m.raw" "$work/cross.$m.raw")"
        if [ "$nd" = 0 ]; then bdec="identical"; else bdec="$nd samples, max $me, ${sn} dB"; fail=1; fi
        read -r nd _ me sn _ <<< "$("$work/ours" cmp "$work/ours.$m.raw" "$work/theirs.$m.raw")"
        if [ "$nd" = 0 ]; then bown="identical"; else bown="$nd samples, max $me, ${sn} dB"; fi
        printf '%-5s %-6s %-26s %-26s %s\n' "$m" "${names[$m]}" "$benc" "$bdec" "$bown"
    done
fi

if [ "$fail" = 0 ]; then echo "PASS"; else echo "FAILED"; fi
exit "$fail"
