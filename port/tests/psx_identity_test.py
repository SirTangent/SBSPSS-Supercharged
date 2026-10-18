"""The rules port/tools/psx_identity.py checks by, run on fixed cases.

`psx_identity.py lines` decides whether the PS1 compiler sees the same tokens
before and after an edit, expanding the macro patterns psx_byte_identity.md
recommends; `build` decides whether a revision has the line-insensitive
DEBUG plumbing by matching makefile.gaz.  A rule that passes a real change
lets a PS1 difference through unnoticed until someone runs `build`, so each
case below is one the rules must get right, including the counterexamples
from PR #91's review (issue #89).  Needs nothing but the tool's source: no
git, no build, no data.

    psx_identity_test.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import psx_identity as tool  # noqa: E402


def lines_ok(old, new):
    """What `lines` decides for one file: True for ok, False for DIFF."""
    old_view, _ = tool.ps1_view(old)
    new_view, _ = tool.ps1_view(new)
    if [t for _, t in old_view] == [t for _, t in new_view]:
        return True
    try:
        old_tok, new_tok = tool.ps1_tokens(old_view, new_view)
    except tool.Unknown:
        return False
    return old_tok == new_tok


# A macro used the way #50/#81's PROMPT_ICONS is: one copy, #undef'd after.
ICONS_OLD = """\
void f(int cfg)
{
#define ICONS(_cfg,Icon) int Button=get(_cfg); int Icon[2]={-1,-1}; if(Button) Icon[0]=frame(Button);
	ICONS(cfg,Icon)
	draw(Icon[0]);
}
#undef ICONS
"""

# (name, base text, new text, True if `lines` must say ok)
LINES = [
    ("identical text", "int a=1;\n", "int a=1;\n", True),
    ("a comment edit", "int a=1; // one\n", "int a=1; /* uno */\n// more\n", True),
    ("a PC-only arm", "int a=1;\n",
     "#if !defined(PSX_MIPS_ASM)\nint pc=2;\n#endif\nint a=1;\n", True),
    ("PC_PS1 hands the PS1 its old value", "int a=3;\n", "int a=PC_PS1(2,3);\n", True),
    ("PC_PS1 with a changed PS1 value", "int a=3;\n", "int a=PC_PS1(2,4);\n", False),
    ("a new macro, the written-out code shared",
     "int a=x+1;\nint b=y+1;\n",
     "#define INC(v) v+1\nint a=INC(x);\nint b=INC(y);\n#undef INC\n", True),
    ("a new macro whose use differs",
     "int a=x+1;\n", "#define INC(v) v+2\nint a=INC(x);\n#undef INC\n", False),
    ("a new macro used in an #if", "#if L > 2\nint a;\n#endif\n",
     "#define L 3\n#if L > 2\nint a;\n#endif\n#undef L\n", False),

    # A redefined macro (issue #89's PROMPT_ICONS gaining Button)
    ("a file-local macro gains a parameter", ICONS_OLD,
     ICONS_OLD.replace("ICONS(_cfg,Icon) int Button", "ICONS(_cfg,Button,Icon) int Button")
              .replace("ICONS(cfg,Icon)", "ICONS(cfg,Button,Icon)"), True),
    ("the same, a call passing another name", ICONS_OLD,
     ICONS_OLD.replace("ICONS(_cfg,Icon) int Button", "ICONS(_cfg,Button,Icon) int Button")
              .replace("ICONS(cfg,Icon)", "ICONS(cfg,Btn,Icon)"), False),
    ("a file-local macro's body changed", ICONS_OLD,
     ICONS_OLD.replace("{-1,-1}", "{-1,0}"), False),
    ("a file-local macro's #undef moved above its use", ICONS_OLD,
     ICONS_OLD.replace("#undef ICONS\n", "").replace("\tICONS(cfg,Icon)", "#undef ICONS\n\tICONS(cfg,Icon)"),
     False),
    ("a redefined macro with no #undef", ICONS_OLD.replace("#undef ICONS\n", ""),
     ICONS_OLD.replace("#undef ICONS\n", "").replace("ICONS(_cfg,Icon) int Button", "ICONS(_cfg,Button,Icon) int Button")
              .replace("ICONS(cfg,Icon)", "ICONS(cfg,Button,Icon)"), False),
    ("a header redefines a macro it doesn't use", "#define X 3\n", "#define X 4\n", False),
    ("a header redefines a macro it uses", "#define X 3\nint a=X;\n", "#define X (1+2)\nint a=X;\n", False),

    # PR #91 review counterexamples: each one passed the first version of the rule
    ("review 1: used through another macro's body",
     "#define N 3\n#define M (N+1)\nint a=M;\n#undef N\n#undef M\n",
     "#define N 4\n#define M (N+1)\nint a=M;\n#undef N\n#undef M\n", False),
    ("review 1: used in an #if",
     "#define LIMIT 3\n#if LIMIT > 2\nint a;\n#else\nint b;\n#endif\n#undef LIMIT\n",
     "#define LIMIT 1\n#if LIMIT > 2\nint a;\n#else\nint b;\n#endif\n#undef LIMIT\n", False),
    ("review 2: a conditional #undef",
     "#define X 3\n#ifdef SOMETHING\n#undef X\n#endif\nint a=X;\n",
     "#define X 4\n#ifdef SOMETHING\n#undef X\n#endif\nint a=X;\n", False),
    ("review 2: an #undef before the #define",
     "#undef X\n#define X 3\nint a=X;\n", "#undef X\n#define X 4\nint a=X;\n", False),
    ("review 3: an X-macro's uses in an included file",
     "#define ENTRY(a) a,\nint t[]={\n#include \"t.inc\"\n};\n#undef ENTRY\n",
     "#define ENTRY(a) a+1,\nint t[]={\n#include \"t.inc\"\n};\n#undef ENTRY\n", False),
]

# The makefile.gaz plumbing knows_noline looks for: every spelling of the
# ifeq make accepts must match, and nothing else may.
NOLINE_YES = [
    "ifeq ($(origin SBSP_IDENTITY_NOLINE),command line)",
    "ifeq ( $(origin SBSP_IDENTITY_NOLINE) , command line )",
    "  ifeq ($(origin  SBSP_IDENTITY_NOLINE),command line)",
    "ifeq\t($(origin SBSP_IDENTITY_NOLINE),command\tline)",
    "ifeq (command line,$(origin SBSP_IDENTITY_NOLINE))",
    'ifeq "$(origin SBSP_IDENTITY_NOLINE)" "command line"',
    "ifeq '$(origin SBSP_IDENTITY_NOLINE)' 'command line'",
    'ifeq "command line" "$(origin SBSP_IDENTITY_NOLINE)"',
    "ifeq (${origin SBSP_IDENTITY_NOLINE},command line)",
    "else ifeq ($(origin SBSP_IDENTITY_NOLINE),command line)",
]
NOLINE_NO = [
    "ifneq ($(origin SBSP_IDENTITY_NOLINE),command line)",
    "ifeq ($(origin SBSP_IDENTITY_NOLINE),environment)",
    "ifeq ($(origin SBSP_IDENTITY_NOLINE),environment) # not command line",
    "ifeq ($(origin SBSP_IDENTITY_NOLINEX),command line)",
    "ifeq ($(origin OTHER),command line)",
    "ifeq ($(origin SBSP_IDENTITY_NOLINE),command line_x)",
    "ifeq ($(origin SBSP_IDENTITY_NOLINE),\ncommand line)",
    "ifeq ($(origin SBSP_IDENTITY_NOLINE),command\nline)",
    "ifeq \"$(origin SBSP_IDENTITY_NOLINE)' 'command line'",
    "ifeq ($(SBSP_IDENTITY_NOLINE),1)",
    "FLAGS += ifeq ($(origin SBSP_IDENTITY_NOLINE),command line)",
]


def main():
    failed = 0
    for name, old, new, want in LINES:
        got = lines_ok(old, new)
        ok = got == want
        failed += not ok
        print("%s  lines: %s -> %s" % ("pass" if ok else "FAIL", name, "ok" if got else "DIFF"))
    pattern = tool.NOLINE_PLUMBING["makefile.gaz"][0]
    for text, want in [(t, True) for t in NOLINE_YES] + [(t, False) for t in NOLINE_NO]:
        got = re.search(pattern, text, re.M) is not None
        ok = got == want
        failed += not ok
        print("%s  noline: %r %s" % ("pass" if ok else "FAIL", text, "matches" if got else "no match"))
    print("%d failed" % failed if failed else "all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
