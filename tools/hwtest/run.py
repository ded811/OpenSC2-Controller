#!/usr/bin/env python3
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# run.py: the entry point. `python -I tools/hwtest/run.py [options]`. With -I the script's own
# folder is left off sys.path, so this puts tools/ on it and imports the hwtest package, then exits
# with the CLI's code. See tools/hwtest/README.md.
import os
import sys


def _main():
    tools_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    from hwtest import cli
    return cli.main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(_main())
