#!/bin/sh
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# Build and run the host tests on a PC with gcc: `sh tests/host/run.sh` (CC=clang selects clang,
# which is not tested). Two binaries: "plain" (the library as it is built without the hardware
# test hooks) and "hooks" (CONFIG_OPENSC2_TEST_HOOKS, with the hooks' cases). Both run every
# other case, and both write the transcript of the unarmed command channel; the run fails when the
# two differ by one byte.
# A third binary, the firmware emulator (fwemu.c: the hooks binary with a stdin/stdout main), lets
# the hardware test suite's runner run its groups against the library's own code: when python3 is
# on PATH, its self-tests and its dry run against the emulator run last (docs/testing.md), with
# the self-tests of tools/fake_puck.py and tools/match_steam_version.py.
# NO_RUNNER=1 sh tests/host/run.sh runs the C tests only.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
lib="$root/lib/opensc2"
out="${TMPDIR:-/tmp}"

compile() {
	${CC:-gcc} -std=c11 -Wall -Wextra -Wshadow -Werror -I"$here/shim" -I"$root/include" -I"$lib" \
		-DCONFIG_OPENSC2_CAL=1 -DCONFIG_OPENSC2_CAL_PRESSURE_CLICK=1 -DCONFIG_OPENSC2_PAD_RULES=1 \
		-DCONFIG_OPENSC2_DOCK_LINK=1 \
		"$here/stubs.c" "$here/cal_host.c" "$lib/codec.c" "$lib/lizard.c" \
		"$lib/settings_defaults.c" "$lib/fusion.c" "$lib/usb_desc.c" "$lib/usb_rules.c" \
		"$lib/mode_rules.c" "$lib/retained.c" "$lib/id_rules.c" "$lib/commands.c" \
		"$lib/report_rules.c" "$lib/test_core.c" "$lib/proto_rules.c" "$lib/key_rules.c" \
		"$lib/crash_rules.c" "$lib/input_rules.c" "$lib/report_queue.c" \
		"$lib/transport_rules.c" "$lib/ble_rules.c" "$lib/radio_rules.c" \
		"$lib/cal.c" "$lib/sensor_helpers.c" "$lib/gauge.c" "$lib/charger.c" "$lib/cal_store.c" \
		"$lib/pad.c" "$lib/pad_rules.c" "$lib/imu.c" "$lib/imu_rules.c" \
		"$lib/sys_rules.c" "$lib/chord_rules.c" "$lib/idle_rules.c" "$lib/led_rules.c" \
		"$lib/test_sys.c" "$lib/haptics_dsp.c" "$lib/haptics_tables.c" \
		"$lib/haptics_core.c" "$lib/haptics_rules.c" "$lib/haptics_sounds.c" \
		"$lib/haptics_run.c" "$lib/haptics_help.c" \
		"$lib/dock_codec.c" "$lib/dock_rules.c" "$lib/dock_link.c" "$here/dock_stub.c" "$@" -lm
}

compile "$here/run_tests.c" -o "$out/opensc2_host_tests_plain"
compile "$here/run_tests.c" -DCONFIG_OPENSC2_TEST_HOOKS=1 -DCONFIG_OPENSC2_TEST_RING_SIZE=512 \
	-DCONFIG_OPENSC2_TEST_SEQ_DEPTH=4 "$lib/test_notes.c" "$lib/test_sys_notes.c" \
	"$lib/test_dock.c" "$here/hooks_stub.c" -o "$out/opensc2_host_tests_hooks"
# the emulator: the Kconfig defaults of the app (a 2 KB ring, 8 sequence steps), the system state
# machine, the haptics synthesiser as app/haptics.conf has it, and the dock link (on with the hooks)
compile "$here/fwemu.c" "$here/fwemu_sys.c" -DOSC2_HOST_FWEMU -DCONFIG_OPENSC2_SYSTEM=1 \
	-DCONFIG_OPENSC2_TEST_HOOKS=1 -DCONFIG_OPENSC2_TEST_RING_SIZE=2048 \
	-DCONFIG_OPENSC2_TEST_SEQ_DEPTH=8 -DCONFIG_OPENSC2_HAPTICS_SYNTH=1 \
	"$lib/test_notes.c" "$lib/test_sys_notes.c" "$lib/test_dock.c" "$here/hooks_stub.c" \
	"$here/haptics_emu.c" -o "$out/opensc2_fwemu"
# Without CONFIG_OPENSC2_CAL the calibration's calls are stand-ins: those files must still compile.
for f in "$lib/commands.c" "$lib/cal_store.c" "$lib/test_core.c" "$lib/pad.c"; do
	${CC:-gcc} -std=c11 -Wall -Wextra -Wshadow -Werror -I"$here/shim" -I"$root/include" -I"$lib" \
		-fsyntax-only "$f"
done

rc=0
"$out/opensc2_host_tests_plain" --vectors "$root/tests/vectors/hwtest" \
	--transcript "$out/opensc2_host_transcript_plain.txt" || rc=1
"$out/opensc2_host_tests_hooks" --vectors "$root/tests/vectors/hwtest" \
	--transcript "$out/opensc2_host_transcript_hooks.txt" || rc=1

# The unarmed command channel, byte for byte: both binaries must have answered alike.
if cmp -s "$out/opensc2_host_transcript_plain.txt" "$out/opensc2_host_transcript_hooks.txt"; then
	echo "PASS the two binaries' transcripts are the same"
else
	echo "FAIL cmd_differential: the transcripts of the two binaries differ"
	diff "$out/opensc2_host_transcript_plain.txt" "$out/opensc2_host_transcript_hooks.txt" |
		head -n 8 || true
	rc=1
fi

# The hardware test suite's runner: its self-tests (with the emulator) and a dry run against the
# emulator, which must pass with nothing failing. No device is opened.
py=$(command -v python3 || command -v python || true)
emu="$out/opensc2_fwemu"
[ -f "$emu.exe" ] && emu="$emu.exe"      # gcc on Windows (MSYS2) adds the suffix
if [ -n "${NO_RUNNER:-}" ]; then
	echo "SKIP the runner (NO_RUNNER)"
elif [ -z "$py" ]; then
	echo "SKIP the runner: no python3 on PATH"
else
	OPENSC2_FWEMU="$emu" "$py" -I "$root/tools/hwtest/run.py" --selftest ||
		{ echo "FAIL the runner self-tests"; rc=1; }
	"$py" -I "$root/tools/hwtest/run.py" --dry-run --fw-emu "$emu" --lizard --dock --yes \
		--results "$out/opensc2_hwtest_results" ||
		{ echo "FAIL the dry run against the emulator"; rc=1; }
	# ... and with --flash: the flash group and the tests that store something (system.keylock)
	"$py" -I "$root/tools/hwtest/run.py" --dry-run --fw-emu "$emu" --flash --lizard --dock \
		--yes --results "$out/opensc2_hwtest_results" ||
		{ echo "FAIL the --flash dry run against the emulator"; rc=1; }
	OPENSC2_FWEMU="$emu" "$py" -I "$root/tools/fake_puck.py" --selftest ||
		{ echo "FAIL the fake_puck.py self-tests"; rc=1; }
	"$py" -I "$root/tools/match_steam_version.py" --selftest ||
		{ echo "FAIL the match_steam_version.py self-tests"; rc=1; }
fi
exit $rc
