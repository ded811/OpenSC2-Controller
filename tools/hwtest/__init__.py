# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# hwtest: the PC side of OpenSC2's hardware test suite (see tools/hwtest/README.md). Checks every
# feature the library emulates, with no physical interaction, by arming the firmware's test hooks
# and reading the board's own reports, the puck's slots (read-only) and the board's console.
#
# Every module but hwtest.win imports on any OS, so the self-tests and the dry run run anywhere.
__all__ = ["cli"]
