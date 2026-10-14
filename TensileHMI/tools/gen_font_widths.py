#!/usr/bin/env python3
"""Generate TensileHMI/components/tensile_report/core/tr_font_widths.c.

Glyph advance widths (1/1000 em) for the base-14 fonts Helvetica and
Helvetica-Bold, indexed by WinAnsiEncoding code 32..255.

Source: reportlab (BSD license, Copyright ReportLab Inc.), module
reportlab.pdfbase._fontdata, which carries the Adobe Core-14 AFM metrics
(the AFM files themselves are freely redistributable per Adobe's notice).

Usage:
    python3 -m venv /tmp/venv && /tmp/venv/bin/pip install reportlab
    /tmp/venv/bin/python TensileHMI/tools/gen_font_widths.py [out.c]
"""
import os
import sys

import reportlab
from reportlab.pdfbase import _fontdata

FONTS = [("Helvetica", "TR_FONT_HELVETICA"), ("Helvetica-Bold", "TR_FONT_HELVETICA_BOLD")]
FIRST, LAST = 32, 255

# WinAnsi codes that are undefined in CP1252. reportlab maps them to 'bullet';
# tensile_report never emits them, so give them width 0 to make that explicit.
UNDEFINED = {0x7F, 0x81, 0x8D, 0x8F, 0x90, 0x9D}


def table_for(font):
    enc = _fontdata.encodings["WinAnsiEncoding"]
    widths = _fontdata.widthsByFontGlyph[font]
    out = []
    for code in range(FIRST, LAST + 1):
        if code in UNDEFINED:
            out.append(0)
            continue
        glyph = enc[code]
        if glyph is None or glyph not in widths:
            raise SystemExit(f"{font}: no width for code {code:#x} ({glyph})")
        out.append(int(widths[glyph]))
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.join(here, "..", "components", "tensile_report", "core", "tr_font_widths.c")
    path = sys.argv[1] if len(sys.argv) > 1 else default
    lines = [
        "/*",
        " * GENERATED FILE - do not edit. Regenerate with TensileHMI/tools/gen_font_widths.py",
        " *",
        " * Glyph advance widths in 1/1000 em for the PDF base-14 fonts Helvetica and",
        " * Helvetica-Bold, indexed by WinAnsiEncoding code (32..255).",
        " *",
        f" * Source: reportlab {reportlab.Version}, reportlab.pdfbase._fontdata",
        " *   (BSD 3-clause license, Copyright (c) 2000-2024 ReportLab Inc.), which",
        " *   carries the Adobe Core-14 AFM font metrics. The AFM metric data is",
        " *   distributed by Adobe with permission to copy, use and distribute.",
        " * Undefined CP1252 codes (0x7F 0x81 0x8D 0x8F 0x90 0x9D) are 0.",
        " */",
        '#include "tr_internal.h"',
        "",
        f"const uint16_t tr_font_widths[TR_FONT_COUNT][{LAST - FIRST + 1}] = {{",
    ]
    for font, enum in FONTS:
        t = table_for(font)
        lines.append(f"    [{enum}] = {{ /* {font} */")
        for i in range(0, len(t), 16):
            chunk = ", ".join(f"{w:4d}" for w in t[i:i + 16])
            lines.append(f"        {chunk}, /* 0x{FIRST + i:02X} */")
        lines.append("    },")
    lines.append("};")
    with open(path, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
