#!/usr/bin/env python3
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# collect_licenses.py: write THIRD_PARTY_LICENSES.txt for a release, the licence texts of every
# component THIRD_PARTY.md lists, in one file with a header per component. OpenSC2's own LICENSE
# comes first. The texts come from the nRF Connect SDK the image was built with, read with
# `git -C <ncs>/<repo> show HEAD:<path>`: read only, nothing in the SDK is changed. For nrfx and the
# nRF MDK, whose licence is in each file's header, it takes the first comment block of one file each.
#
#   python -I tools/collect_licenses.py --ncs C:\ncs\v3.4.0 --out C:\b\rel\THIRD_PARTY_LICENSES.txt
#
# Standard library only; needs git on PATH. Exit codes: 0 written, 1 a text could not be read
# (nothing written), 4 bad options.
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (component, licence, repository in the SDK, path in it, how to take it); in THIRD_PARTY.md order.
COMPONENTS = (
    ("Zephyr RTOS (the kernel, drivers, the USB device stack, CDC-ACM, the input subsystem, the "
     "sensor API, the shell, logging, settings, NVS, the nrfx glue, the promicro_nrf52840 board files)",
     "Apache-2.0", "zephyr", "LICENSE", "file"),
    ("nRF Connect SDK code (Enhanced ShockBurst, the MPSL glue, the CryptoCell entropy driver, the "
     "multithreading lock)",
     "LicenseRef-Nordic-5-Clause", "nrf", "LICENSE", "file"),
    ("MPSL, Nordic's binary Multiprotocol Service Layer library, and its front-end module library: "
     "the licence tag",
     "LicenseRef-Nordic-5-Clause", "nrfxlib", "mpsl/license.txt", "file"),
    ("MPSL: the attribution notice it carries",
     "BSD-3-Clause (ARM Ltd)", "nrfxlib", "mpsl/LICENSE-ATTRIBUTION.txt", "file"),
    ("nrf_cc310_platform, Nordic's binary CryptoCell 310 platform library: the licence tag",
     "LicenseRef-Nordic-5-Clause", "nrfxlib", "crypto/nrf_cc310_platform/license.txt", "file"),
    ("nrfxlib (MPSL and nrf_cc310_platform): the licence text the two tags above name",
     "LicenseRef-Nordic-5-Clause", "nrfxlib", "LICENSE", "file"),
    ("nrfx drivers (the licence in the header of each file; this is nrfx/nrfx.h's)",
     "BSD-3-Clause", "modules/hal/nordic", "nrfx/nrfx.h", "header"),
    ("The nRF MDK (the licence in the header of each file; this is system_nrf52840.c's)",
     "BSD-3-Clause", "modules/hal/nordic", "nrfx/bsp/stable/mdk/system_nrf52840.c", "header"),
    ("CMSIS 6 core headers",
     "Apache-2.0", "modules/hal/cmsis_6", "LICENSE", "file"),
    ("picolibc, the C library (the toolchain's build of it)",
     "BSD-style licences, listed file by file", "modules/lib/picolibc", "COPYING.picolibc", "file"),
)

LIBGCC_NOTE = (
    "libgcc, GCC's runtime library, comes with the toolchain (the Zephyr SDK inside the nRF Connect\n"
    "SDK's toolchain bundle), not with the SDK's repositories. Its licence is the GNU General Public\n"
    "License, version 3, with the GCC Runtime Library Exception, version 3.1:\n"
    "https://www.gnu.org/licenses/gpl-3.0.html and https://www.gnu.org/licenses/gcc-exception-3.1.html\n")

RULE = "=" * 100


class Missing(Exception):
    """A licence text could not be read."""


def git(repo_dir, *args):
    """Run a read-only git command in repo_dir and return its standard output as text."""
    try:
        done = subprocess.run(["git", "-C", repo_dir] + list(args), capture_output=True, check=False)
    except OSError as exc:
        raise Missing("cannot run git: %s" % exc) from exc
    if done.returncode != 0:
        raise Missing("git -C %s %s: %s" % (repo_dir, " ".join(args),
                                            done.stderr.decode("utf-8", "replace").strip()))
    return done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")


def first_comment_block(text):
    """The first /* ... */ block of a C file, without the comment markers."""
    start = text.find("/*")
    end = text.find("*/", start + 2)
    if start < 0 or end < 0:
        raise Missing("no comment block found")
    lines = []
    for line in text[start + 2:end].split("\n"):
        stripped = line.strip()
        if stripped.startswith("*"):
            stripped = stripped[1:]
            if stripped.startswith(" "):
                stripped = stripped[1:]
        lines.append(stripped.rstrip())
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines) + "\n"


def section(title, licence, source, text):
    return "%s\n%s\nLicence: %s\nSource: %s\n%s\n\n%s\n" % (RULE, title, licence, source, RULE,
                                                         text.rstrip("\n"))


def collect(ncs):
    """Every section of the file, in order, as text."""
    if not os.path.isdir(os.path.join(ncs, "zephyr")) or not os.path.isdir(os.path.join(ncs, "nrf")):
        raise Missing("%s does not look like an nRF Connect SDK folder (no zephyr/ and nrf/)" % ncs)
    try:
        with open(os.path.join(ROOT, "LICENSE"), encoding="utf-8") as f:
            own = f.read()
    except OSError as exc:
        raise Missing("cannot read OpenSC2's LICENSE: %s" % exc) from exc

    version = git(os.path.join(ncs, "nrf"), "describe", "--tags", "--always").strip()
    parts = [
        "Third-party licences of a built OpenSC2 image\n\n"
        "OpenSC2-Controller: https://github.com/ded811/OpenSC2-Controller\n"
        "The texts below were collected from nRF Connect SDK %s with tools/collect_licenses.py.\n"
        "THIRD_PARTY.md in the repository lists the components and says which part of an image\n"
        "each one is. NOTICE.md says what the USB IDs and the names Steam and Steam Controller mean\n"
        "here.\n" % version,
        section("OpenSC2 (the library, the firmware, the samples, the template)", "MIT",
                "LICENSE in the OpenSC2-Controller repository", own),
    ]
    for title, licence, repo, path, how in COMPONENTS:
        repo_dir = os.path.join(ncs, *repo.split("/"))
        text = git(repo_dir, "show", "HEAD:" + path)
        if how == "header":
            text = first_comment_block(text)
        if not text.strip():
            raise Missing("%s/%s is empty" % (repo, path))
        commit = git(repo_dir, "rev-parse", "--short=12", "HEAD").strip()
        parts.append(section(title, licence, "%s/%s (commit %s)" % (repo, path, commit), text))
    parts.append(section("libgcc, GCC's runtime library", "GPL-3.0 with the GCC Runtime Library "
                         "Exception 3.1", "the toolchain", LIBGCC_NOTE))
    return "\n".join(parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Write THIRD_PARTY_LICENSES.txt for a release from the "
                                 "nRF Connect SDK the image was built with (read only).")
    ap.add_argument("--ncs", required=True,
                    help=r"the SDK folder, for example C:\ncs\v3.4.0 or ~/ncs/v3.4.0")
    ap.add_argument("--out", required=True, help="the file to write")
    try:
        args = ap.parse_args(argv)
    except SystemExit as exc:
        return 4 if exc.code else 0
    ncs = os.path.abspath(os.path.expanduser(args.ncs))
    try:
        text = collect(ncs)
    except Missing as exc:
        print("collect_licenses: %s; nothing written" % exc, file=sys.stderr)
        return 1
    out = os.path.abspath(os.path.expanduser(args.out))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote %s (%d components, %d bytes)" % (out, len(COMPONENTS) + 2, len(text.encode("utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
