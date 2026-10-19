#!/usr/bin/env python3
# Copyright (c) 2026 the OpenSC2-Controller authors
# SPDX-License-Identifier: MIT
#
# check_links.py: check the links in the repository's Markdown files, the way GitHub resolves them.
#
#   python -I tools/check_links.py                       # every relative link and anchor
#   python -I tools/check_links.py --research <folder>   # also the research repository's links
#   python -I tools/check_links.py --selftest            # its own tests
#
# Every *.md file is read, except under .git/, build folders and tools/hwtest/results/. For each
# relative link (and each absolute link to this repository's own files) the target must exist,
# with the same upper and lower case as on disk (GitHub is case sensitive, Windows is not), and an
# anchor (#...) must be a heading of the target page, as GitHub turns headings into anchors:
# lower case, punctuation other than - and _ removed, spaces to -, inline code without its
# backticks, and -1, -2, ... after repeated headings. Links inside code blocks and inline code are
# not links. Other absolute links are only counted. With --research, links to
# https://github.com/ded811/steam-controller-2-reverse-engineered/blob/main/<page>#<anchor> are
# checked against a local checkout of that repository.
#
# Prints file:line for every broken link and exits with 1 if there is one. Standard library only.
import argparse
import os
import re
import sys
import tempfile
import unicodedata
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_URL = "https://github.com/ded811/OpenSC2-Controller"
RESEARCH_URL = "https://github.com/ded811/steam-controller-2-reverse-engineered"

SKIP_DIRS = {".git", "build", "__pycache__", ".venv", "venv", ".cache", "node_modules"}
SKIP_DIR_PREFIXES = ("build-", "twister-out")
SKIP_REL_DIRS = {"tools/hwtest/results"}

# ---- GitHub's heading anchors ------------------------------------------------------------------


def slug(text):
    """GitHub's anchor for a heading's text: lower case; letters, marks, digits, '_', '-' and
    spaces are kept and everything else is dropped; each space becomes '-'."""
    out = []
    for ch in text.strip().lower():
        if ch == " " or ch == "-":
            out.append("-" if ch == " " else ch)
        elif unicodedata.category(ch)[0] in "LMN" or unicodedata.category(ch) == "Pc":
            out.append(ch)
    return "".join(out)


_INLINE_CODE = re.compile(r"(`+)(.+?)\1")
_IMAGE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
_LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")
_REF_LINK = re.compile(r"\[([^\]]*)\]\[[^\]]*\]")
_HTML_TAG = re.compile(r"<[^>]+>")
_EMPHASIS = re.compile(r"(\*\*|__|\*|(?<![0-9A-Za-z])_)(?=\S)(.+?)(?<=\S)\1(?![0-9A-Za-z])")


def heading_text(raw):
    """The text GitHub renders for a heading's Markdown: code without backticks, links and images
    as their text, emphasis markers and HTML tags dropped."""
    codes = []

    def keep_code(m):
        codes.append(m.group(2).strip() if m.group(2).strip() else m.group(2))
        return "\0%d\0" % (len(codes) - 1)

    t = _INLINE_CODE.sub(keep_code, raw)
    t = _IMAGE.sub(r"\1", t)
    t = _LINK.sub(r"\1", t)
    t = _REF_LINK.sub(r"\1", t)
    t = _HTML_TAG.sub("", t)
    for _ in range(3):
        t = _EMPHASIS.sub(r"\2", t)
    return re.sub("\0(\\d+)\0", lambda m: codes[int(m.group(1))], t)


_ATX = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
_FENCE = re.compile(r"^[ 	]*(`{3,}|~{3,})")
_HTML_ID = re.compile(r"<[a-zA-Z][^>]*?\s(?:id|name)\s*=\s*[\"']([^\"']+)[\"']")


def fenced_lines(lines):
    """For each line: True when it is inside a fenced code block (the fences included)."""
    inside, fence, flags = False, "", []
    for line in lines:
        m = _FENCE.match(line)
        if not inside and m:
            inside, fence = True, m.group(1)
            flags.append(True)
            continue
        if inside:
            flags.append(True)
            stripped = line.strip()
            if stripped.startswith(fence[0] * len(fence)) and set(stripped) <= {fence[0]}:
                inside = False
            continue
        flags.append(False)
    return flags


def anchors_in(text):
    """Every anchor a Markdown page has: its headings' slugs and the ids of its HTML tags."""
    lines = text.split("\n")
    seen, anchors = {}, set()
    for line, fenced in zip(lines, fenced_lines(lines)):
        if fenced:
            continue
        m = _ATX.match(line)
        if m:
            base = slug(heading_text(m.group(2) or ""))
            n = seen.get(base, 0)
            seen[base] = n + 1
            anchor = base if n == 0 else "%s-%d" % (base, n)
            while anchor in anchors:          # a heading that already ends in -1
                n += 1
                anchor = "%s-%d" % (base, n)
            anchors.add(anchor)
        for hid in _HTML_ID.findall(line):
            anchors.add(hid)
    return anchors


# ---- links ---------------------------------------------------------------------------------------


def masked(text):
    """The text with code blocks, inline code and HTML comments blanked out (newlines kept), so
    that what looks like a link inside them is not taken for one."""
    lines = text.split("\n")
    flags = fenced_lines(lines)
    text = "\n".join(" " * len(line) if fenced else line for line, fenced in zip(lines, flags))

    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))

    text = re.sub(r"<!--.*?-->", blank, text, flags=re.S)
    return re.sub(r"(?<!`)(`+)(?!`)(?:(?!\n[ \t]*\n).)+?(?<!`)\1(?!`)", blank, text, flags=re.S)


_MD_LINK = re.compile(
    r"!?\[(?:[^\[\]]|\[[^\[\]]*\])*\]\(\s*(<[^>\n]*>|(?:[^()\s]|\([^()\s]*\))*)"
    r"(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?\s*\)")
_REF_DEF = re.compile(r"^ {0,3}\[[^\]]+\]:[ \t]*(<[^>\n]*>|\S+)", re.M)
_HTML_LINK = re.compile(r"<(?:a|img)\s[^>]*?(?:href|src)\s*=\s*[\"']([^\"']+)[\"']", re.I)


def links_in(text):
    """(line, target) for every link and image in the page, in order."""
    m_text = masked(text)
    found = []
    for rx in (_MD_LINK, _REF_DEF, _HTML_LINK):
        for m in rx.finditer(m_text):
            target = m.group(1)
            found.append((m_text.count("\n", 0, m.start(1)) + 1, target))
    found.sort(key=lambda x: x[0])
    return found


# ---- checking ------------------------------------------------------------------------------------


_LISTINGS = {}


def exact_case(path, top):
    """True when every part of `path` below the folder `top` has the same case on disk."""
    path, top = os.path.normpath(path), os.path.normpath(top)
    while path != top:
        parent, name = os.path.split(path)
        if not name or parent == path:
            return True
        if parent not in _LISTINGS:
            try:
                _LISTINGS[parent] = set(os.listdir(parent))
            except OSError:
                _LISTINGS[parent] = set()
        if name not in _LISTINGS[parent]:
            return False
        path = parent
    return True


class Checker:
    def __init__(self, root, research=None):
        self.root = os.path.abspath(root)
        self.research = os.path.abspath(research) if research else None
        self.anchor_cache = {}
        self.problems = []
        self.counts = {"files": 0, "relative": 0, "anchors": 0, "absolute": 0, "research": 0,
                       "research_skipped": 0}

    def anchors_of(self, path):
        if path not in self.anchor_cache:
            with open(path, encoding="utf-8", errors="replace") as f:
                self.anchor_cache[path] = anchors_in(f.read())
        return self.anchor_cache[path]

    def rel(self, path):
        return os.path.relpath(path, self.root).replace(os.sep, "/")

    def problem(self, md, line, target, why):
        self.problems.append((self.rel(md), line, target, why))

    def check_target(self, md, line, target, base_dir, path, anchor, limit):
        """A path (already resolved against base_dir) and an optional anchor."""
        full = os.path.normpath(os.path.join(base_dir, path)) if path else md
        if limit and os.path.commonpath([full, limit]) != limit:
            self.problem(md, line, target, "points outside the repository")
            return
        if not os.path.exists(full):
            self.problem(md, line, target, "no such file")
            return
        if not exact_case(full, limit or os.path.dirname(full)):
            self.problem(md, line, target, "the file exists only in another upper/lower case")
            return
        if not anchor:
            return
        page = full
        if os.path.isdir(full):
            page = os.path.join(full, "README.md")
            if not os.path.isfile(page):
                self.problem(md, line, target, "an anchor on a folder without a README.md")
                return
        if not page.lower().endswith(".md"):
            return                                   # line anchors of source files: not checked
        self.counts["anchors"] += 1
        if anchor not in self.anchors_of(page):
            self.problem(md, line, target, "no heading for #%s in %s" % (anchor, os.path.basename(page)))

    def check_link(self, md, line, raw):
        target = raw.strip()
        if target.startswith("<") and target.endswith(">"):
            target = target[1:-1].strip()
        if not target:
            self.problem(md, line, raw, "an empty link")
            return
        url, _, anchor = target.partition("#")
        anchor = urllib.parse.unquote(anchor)
        if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target):
            for prefix, root, key in ((REPO_URL, self.root, "relative"),
                                      (RESEARCH_URL, self.research, "research")):
                m = re.match(re.escape(prefix) + r"/(?:blob|tree)/[^/]+/([^?#]*)", url)
                if not m:
                    continue
                if root is None:
                    self.counts["research_skipped"] += 1
                    return
                self.counts[key] += 1
                self.check_target(md, line, target, root, urllib.parse.unquote(m.group(1)),
                                  anchor, root)
                return
            self.counts["absolute"] += 1
            return
        path = urllib.parse.unquote(url.split("?", 1)[0])
        self.counts["relative"] += 1
        if path.startswith("/"):
            self.check_target(md, line, target, self.root, path.lstrip("/"), anchor, self.root)
        else:
            self.check_target(md, line, target, os.path.dirname(md), path, anchor, self.root)

    def check_file(self, md):
        self.counts["files"] += 1
        with open(md, encoding="utf-8", errors="replace") as f:
            text = f.read()
        for line, target in links_in(text):
            self.check_link(md, line, target)


def markdown_files(root):
    for here, dirs, files in os.walk(root):
        rel = os.path.relpath(here, root).replace(os.sep, "/")
        dirs[:] = sorted(d for d in dirs
                         if d not in SKIP_DIRS and not d.startswith(SKIP_DIR_PREFIXES)
                         and ((rel + "/" + d) if rel != "." else d) not in SKIP_REL_DIRS)
        for name in sorted(files):
            if name.lower().endswith(".md"):
                yield os.path.join(here, name)


def run(root, research=None, out=sys.stdout):
    checker = Checker(root, research)
    for md in markdown_files(checker.root):
        checker.check_file(md)
    for rel, line, target, why in checker.problems:
        print("%s:%d: %s: %s" % (rel, line, target, why), file=out)
    c = checker.counts
    print("%d files: %d relative links, %d anchors, %d research links checked; %d absolute links "
          "not checked%s; %d broken" %
          (c["files"], c["relative"], c["anchors"], c["research"], c["absolute"],
           (", %d research links not checked (no --research)" % c["research_skipped"])
           if c["research_skipped"] else "", len(checker.problems)), file=out)
    return 1 if checker.problems else 0


# ---- self-test -----------------------------------------------------------------------------------

SLUG_CASES = (
    ("Way 1: a plain folder", "way-1-a-plain-folder"),
    ("The test channel: vendor command `0x7E`", "the-test-channel-vendor-command-0x7e"),
    ("The console: `opensc2 test`", "the-console-opensc2-test"),
    ("Steam's firmware update", "steams-firmware-update"),
    ("What can I share in an issue?", "what-can-i-share-in-an-issue"),
    ("`0x43`: battery", "0x43-battery"),
    ("Test builds only: no host, then wireless", "test-builds-only-no-host-then-wireless"),
    ("Output reports `0x80` to `0x8A`", "output-reports-0x80-to-0x8a"),
    ("The serial console (wireless mode)", "the-serial-console-wireless-mode"),
    ("CONFIG_OPENSC2_TEST_HOOKS", "config_opensc2_test_hooks"),
    ("A -- B", "a----b"),
    ("C++ & Rust", "c--rust"),
    ("Über Größe", "über-größe"),
    ("1. Numbered", "1-numbered"),
    ("**Bold** heading", "bold-heading"),
    ("Heading with [a link](x.md)", "heading-with-a-link"),
    ("Temperature 20 °C, 5 µs", "temperature-20-c-5-µs"),
    ("_emphasis_ and snake_case", "emphasis-and-snake_case"),
)


def selftest():
    failures = []
    for text, want in SLUG_CASES:
        got = slug(heading_text(text))
        if got != want:
            failures.append("slug(%r) = %r, want %r" % (text, got, want))

    dup = anchors_in("# Intro\n\n## Intro\n\n### Intro\n")
    if dup != {"intro", "intro-1", "intro-2"}:
        failures.append("repeated headings: %r" % sorted(dup))
    closed = anchors_in("## Title ##\n```\n# not a heading\n```\n<a id=\"custom\"></a>\n")
    if closed != {"title", "custom"}:
        failures.append("closing hashes, fences, HTML ids: %r" % sorted(closed))

    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "docs", "sub"))
        files = {
            "README.md": "# Top\n\nSee [docs](docs/a.md), [map](docs/), [lic](LICENSE).\n",
            "LICENSE": "MIT\n",
            "docs/a.md": (
                "# Page A\n\n## Section one\n\n"
                "[ok](b.md#target-here) [bad anchor](b.md#nope) [missing](gone.md)\n"
                "[self](#section-one) [self bad](#section-two) [up](../README.md#top)\n"
                "[case](B.md) [web](https://example.com/x) [mail](mailto:a@b.c)\n"
                "`[code](nope.md)` and\n\n```\n[fenced](nope.md)\n```\n"
                "[repo](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/b.md#target-here)\n"
                "[repo bad](https://github.com/ded811/OpenSC2-Controller/blob/main/docs/c.md)\n"
                "[multi\nline](b.md) ![img](sub/pic.png)\n"
                "<!-- [comment](nope.md) -->\n[ref]: b.md#target-here\n"),
            "docs/b.md": "# B\n\n## Target here\n",
            "docs/README.md": "# Docs\n",
            "docs/sub/pic.png": "",
        }
        for rel, body in files.items():
            with open(os.path.join(tmp, *rel.split("/")), "w", encoding="utf-8", newline="\n") as f:
                f.write(body)
        checker = Checker(tmp)
        for md in markdown_files(tmp):
            checker.check_file(md)
        got = sorted((rel, line, target) for rel, line, target, _why in checker.problems)
        want = sorted([
            ("docs/a.md", 5, "b.md#nope"),
            ("docs/a.md", 5, "gone.md"),
            ("docs/a.md", 6, "#section-two"),
            ("docs/a.md", 7, "B.md"),
            ("docs/a.md", 14, "https://github.com/ded811/OpenSC2-Controller/blob/main/docs/c.md"),
        ])
        if got != want:
            failures.append("problems found: %r, want %r" % (got, want))
        if checker.counts["absolute"] != 2:
            failures.append("absolute links counted: %d, want 2" % checker.counts["absolute"])

    for f in failures:
        print("FAIL " + f)
    print("check_links self-test: %d cases, %d failed" % (len(SLUG_CASES) + 3, len(failures)))
    return 1 if failures else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Check the relative links and anchors of the "
                                 "repository's Markdown files.")
    ap.add_argument("--research", metavar="FOLDER",
                    help="a local checkout of steam-controller-2-reverse-engineered: also check "
                         "the links to its pages and their anchors")
    ap.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    ap.add_argument("--selftest", action="store_true", help="run the self-tests and exit")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.research and not os.path.isdir(os.path.join(args.research, "docs")):
        print("check_links: %s has no docs/ folder" % args.research, file=sys.stderr)
        return 4
    return run(args.root, args.research)


if __name__ == "__main__":
    sys.exit(main())
