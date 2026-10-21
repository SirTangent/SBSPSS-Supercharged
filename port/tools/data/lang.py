#!/usr/bin/env python3
"""Merge the translation files into transtext's final.dat (issue #95).

A port of tools/Perl/pl/lang.pl, the 1999 data build's step before
transtext, with its rules:

- A file is lines (CRLF or LF).  `[ID]`, alone on its line up to the
  newline, opens a string; the id may hold only A-Z a-z 0-9 _.  Lines after
  it are read until one is `<prefix>=<text>` for the file's language:
  `;` starts a comment, blank lines are skipped, a line for another
  language is skipped, and a line with no `=` is fatal.  The split is at the
  last `=` (the greedy `^(.*)=(.*)`), so a text holding `=` never matches
  its prefix.  Each line loses its last character before that (lang.pl's
  chop), which is the newline - or the last letter of a final line with
  none.
- The English file is read with prefix `eng`; each other file's prefix is
  its name without `.dat` (`data/translations/swe.dat` -> `swe`).
- A string takes the English text, then each other language's in argument
  order, skipping an empty translation and any id English lacks.  An id
  given twice keeps the text of the later one.
- final.dat is `[ID]`, one `<prefix>=<text>` line per language, and a blank
  line, for each string, with CRLF line ends: transtext drops the last
  character of every line.

Two changes from lang.pl:
- Order.  lang.pl wrote the strings in Perl's hash order, so the STR__
  enum transtext numbers from it - and so trans.h, every script and
  BIGLUMP.BIN - changed on every build unless PERL_HASH_SEED was pinned, and
  even then with the Perl version.  Here the strings come in the order their
  ids first appear in the English file.
- A file that ends inside an `[ID]` before its `<prefix>=` line is an
  error; lang.pl looped forever on it.

    lang.py <english .dat> <final.dat> [<language .dat>...]
"""
import re
import sys

ID_LINE = re.compile(rb"\[(.*)\]\n")
BAD_ID = re.compile(rb"[^a-zA-Z0-9_]")
TEXT_LINE = re.compile(rb"(.*)=(.*)")


class LangError(Exception):
    pass


def lines_of(data):
    """The file's lines with their newlines, a final line without one included."""
    return re.findall(rb"[^\n]*\n|[^\n]+", data)


def read_trans(path, prefix):
    """{id: text} for `prefix`, in the order the ids first appear."""
    with open(path, "rb") as f:
        lines = [l[:-2] + b"\n" if l.endswith(b"\r\n") else l for l in lines_of(f.read())]
    texts = {}
    i = 0
    while i < len(lines):
        m = ID_LINE.match(lines[i])
        i += 1
        if not m:
            continue
        tag = m.group(1)
        if BAD_ID.search(tag):
            raise LangError("Invalid id tag [%s] in %s\nTag may only contain a-z, A-Z, 0-9 and _"
                            % (tag.decode("latin-1"), path))
        while True:
            if i >= len(lines):
                raise LangError("%s ends inside [%s] before its %s=<text> line"
                                % (path, tag.decode("latin-1"), prefix.decode("latin-1")))
            line = re.sub(rb";.*", b"", lines[i][:-1])
            i += 1
            if not line:
                continue
            t = TEXT_LINE.match(line)
            if not t:
                raise LangError("incorrectly formatted file %s\nlooking for %s=<text> for id %s\nline: %s"
                                % (path, prefix.decode("latin-1"), tag.decode("latin-1"),
                                   line.decode("latin-1")))
            if t.group(1) == prefix:
                texts[tag] = t.group(2)
                break
    return texts


def language_prefix(path, previous):
    """lang.pl's prefix: the name between the last \\ or / and `.dat`."""
    m = re.match(r"^.*\\(.*)\.dat", path) or re.match(r"^.*/(.*)\.dat", path)
    return m.group(1).encode("latin-1") if m else previous


def merge(english_path, lang_paths):
    """{id: [b"eng=...", b"<prefix>=...", ...]} in the English file's order."""
    english = read_trans(english_path, b"eng")
    strings = {tag: [b"eng=" + text] for tag, text in english.items()}
    prefix = b""
    for path in lang_paths:
        prefix = language_prefix(path, prefix)
        other = read_trans(path, prefix)
        for tag in english:
            if other.get(tag, b""):
                strings[tag].append(prefix + b"=" + other[tag])
    return strings


def final_dat(strings):
    out = []
    for tag, texts in strings.items():
        out.append(b"[" + tag + b"]\r\n")
        out.extend(t + b"\r\n" for t in texts)
        out.append(b"\r\n")
    return b"".join(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    try:
        data = final_dat(merge(argv[0], argv[2:]))
    except (LangError, OSError) as e:
        print("lang.py: %s" % e, file=sys.stderr)
        return 1
    if argv[1]:
        with open(argv[1], "wb") as f:
            f.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
