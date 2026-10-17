#!/bin/sh
# Build and run the codec tests on a PC with gcc or clang (CC=clang ./run.sh).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
out="${TMPDIR:-/tmp}/opensc2_host_tests"
${CC:-gcc} -std=c11 -Wall -Wextra -Wshadow -Werror -I"$root/include" -I"$root/lib/opensc2" \
	"$here/run_tests.c" "$root/lib/opensc2/codec.c" "$root/lib/opensc2/lizard.c" \
	"$root/lib/opensc2/settings_defaults.c" "$root/lib/opensc2/fusion.c" -lm -o "$out"
"$out"
