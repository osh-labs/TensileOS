#!/usr/bin/env bash
# Validate a PDF produced by tensile_report.
#
#   check_pdf.sh [--pages N] file.pdf [expected text ...]
#
# - qpdf --check must report no errors or warnings
# - Producer must be "TensileOS HMI"
# - every expected string must appear in the extracted text (pdftotext)
# - --pages N: the page count must equal N
#
# Tools: qpdf + poppler-utils (installed with apt-get if missing). If apt is
# unavailable, falls back to Python pikepdf (structure) and pypdf (text).
set -euo pipefail

pages=""
if [[ "${1:-}" == "--pages" ]]; then
    pages="$2"
    shift 2
fi
pdf="${1:?usage: check_pdf.sh [--pages N] file.pdf [expected text ...]}"
shift
[[ -f "$pdf" ]] || { echo "FAIL: $pdf not found"; exit 1; }
expected=("$@")
[[ ${#expected[@]} -gt 0 ]] || expected=("Created via TensileOS")

have() { command -v "$1" >/dev/null 2>&1; }

if ! have qpdf || ! have pdftotext || ! have pdfinfo; then
    if have apt-get; then
        SUDO=""
        [[ $(id -u) -ne 0 ]] && have sudo && SUDO="sudo"
        $SUDO apt-get install -y qpdf poppler-utils >/dev/null 2>&1 ||
            { $SUDO apt-get update >/dev/null 2>&1 && $SUDO apt-get install -y qpdf poppler-utils >/dev/null 2>&1; } || true
    fi
fi

fail=0
text=""

if have qpdf; then
    set +e
    out=$(qpdf --check "$pdf" 2>&1)
    rc=$?
    set -e
    echo "$out" | sed 's/^/qpdf: /'
    if [[ $rc -ne 0 ]] || ! grep -q "No syntax or stream encoding errors found" <<<"$out"; then
        echo "FAIL: qpdf --check (exit $rc)"
        fail=1
    fi
else
    echo "qpdf not available; using pikepdf"
    python3 -m pip install -q pikepdf pypdf >/dev/null 2>&1 || true
    python3 - "$pdf" <<'EOF' || fail=1
import sys, pikepdf
with pikepdf.open(sys.argv[1]) as p:
    problems = p.check()
    for m in problems:
        print("pikepdf:", m)
    print("pikepdf: pages", len(p.pages))
    sys.exit(1 if problems else 0)
EOF
fi

if have pdfinfo; then
    info=$(pdfinfo "$pdf")
    producer=$(sed -n 's/^Producer: *//p' <<<"$info")
    npages=$(sed -n 's/^Pages: *//p' <<<"$info")
else
    read -r producer npages < <(python3 - "$pdf" <<'EOF'
import sys
from pypdf import PdfReader
r = PdfReader(sys.argv[1])
print((r.metadata or {}).get("/Producer", "").replace(" ", "_"), len(r.pages))
EOF
    )
    producer=${producer//_/ }
fi
echo "producer: $producer, pages: $npages"
[[ "$producer" == "TensileOS HMI" ]] || { echo "FAIL: Producer is '$producer'"; fail=1; }
if [[ -n "$pages" && "$npages" != "$pages" ]]; then
    echo "FAIL: expected $pages pages, got $npages"
    fail=1
fi

if have pdftotext; then
    text=$(pdftotext -layout "$pdf" - 2>/dev/null)
else
    text=$(python3 - "$pdf" <<'EOF'
import sys
from pypdf import PdfReader
print("\n".join(p.extract_text() or "" for p in PdfReader(sys.argv[1]).pages))
EOF
    )
fi
for s in "${expected[@]}"; do
    if grep -qF -- "$s" <<<"$text"; then
        echo "ok:   '$s'"
    else
        echo "FAIL: '$s' not found in extracted text"
        fail=1
    fi
done

if [[ $fail -ne 0 ]]; then
    echo "check_pdf: FAILED ($pdf)"
    exit 1
fi
echo "check_pdf: PASSED ($pdf)"
