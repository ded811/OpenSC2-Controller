# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# power.py: keep the PC awake for the length of a run, so it does not sleep in the
# middle and make every device vanish. SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
# on entry, cleared on exit.
import ctypes

_ES_CONTINUOUS = 0x80000000
_ES_SYSTEM_REQUIRED = 0x00000001


def keep_awake():
    ctypes.windll.kernel32.SetThreadExecutionState(_ES_CONTINUOUS | _ES_SYSTEM_REQUIRED)


def allow_sleep():
    ctypes.windll.kernel32.SetThreadExecutionState(_ES_CONTINUOUS)
